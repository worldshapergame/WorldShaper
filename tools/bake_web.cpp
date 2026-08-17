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
#include <thread>
#include <unordered_map>
#include <vector>

// >>> gi
#include "bake/irradiance.hpp"
// <<< gi
// >>> paintexport
#include "bake/paint.hpp"
// <<< paintexport
// >>> ao
#include "bake/occlusion.hpp"
// <<< ao
// >>> probes
#include "bake/probes.hpp"
// <<< probes
// >>> matvol
// The material volume and the thickness field. Header-only, and it lives beside this file rather
// than in src/ because it is the viewer's format and nothing the game builds knows about it.
#include "bake/matvol.hpp"
// <<< matvol
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

// >>> lights
#include "bake/lights.hpp"
// <<< lights

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
    // The same cap for a clip baked as one PART of a manifest, which is a different economy.
    //
    // A fragment is sampled with the whole building's paint stack and intersected with the whole
    // building's solid, so it costs like a small facility rather than like its own box, and there
    // are twenty-eight of them. Measured on the overhauled facility, sampling costs about six
    // times as much for each doubling of resolution: the building alone is 21.7 s at 8 to the
    // metre, 131 s at 16 and about thirteen minutes at 32. Everything at 32 is two and a half
    // hours of a runner; the building at 32 and its parts at 16 is about thirty-five minutes.
    //
    // Which is the right split anyway: the whole clip is the thing you look at, and a fragment is
    // there to answer "is my morning's work the right shape".
    i32 part_metre = 0;   // 0 means "whatever --max-metre says"
    std::string only;   // bake just the clip whose id matches, for working on one
    // Where these clips came from. The site follows whichever branch is being worked on, so the
    // page has to be able to say which one it is showing -- otherwise "it is not showing the
    // overhauled facility" and "it is showing the overhauled facility" look identical.
    std::string branch;
    std::string commit;
    // A stand-in for "the code that turns a clip into a file". CI passes a hash of `src/` and
    // `tools/bake_web.cpp`; change either and every clip is rebaked, which is what has to happen
    // when the sampler or the format moves under them.
    std::string code_hash;
    // Which slice of the clips this run is responsible for, as `index/count`.
    //
    // A cold bake is one runner sampling sixty-three clips in series, and the work divides
    // perfectly: no clip needs any other clip's output. Twelve runners each taking every twelfth
    // file turns half an hour into however long the single slowest clip takes, which is the real
    // floor and is the building itself.
    i32 shard = 0;
    i32 shards = 1;
    bool force = false;        // bake everything, whatever the keys say
    // Write the index over whatever is already there and sample nothing.
    //
    // For getting the SITE up before the clips are ready: the page, the viewer and last run's
    // clips are worth having in the thirty seconds it takes to publish them, rather than after
    // the ten minutes a cold bake of the building takes. Clips that are missing or stale are left
    // out of the index rather than half-written into it, and the real bake replaces the lot.
    bool index_only = false;
    bool verbose = false;
    // >>> paintexport
    // Print the paint stack and the field graph of one ALREADY BAKED clip and do nothing else.
    // Reading the bytes back rather than printing what was about to be written is the point: it
    // checks the layout, the chunk directory and the writer at once, which is what makes it an
    // acceptance test rather than an echo.
    std::string dump_paint;
    // <<< paintexport
};

// The direction the sun is in for every bake, and it is a decision rather than a default.
//
// The facility faces south (-z, `_contract.clip`), so a sun in the south-east at about fifty
// degrees puts light on the elevation the building is judged from, throws the portico's columns
// across the wall behind them, and still reaches down the oculus. A sun overhead lights nothing
// interesting and a sun in the north lights the back of the building.
constexpr f64 kSunDir[3] = {0.42, 0.80, -0.43};

// >>> ao
// How far the ambient-occlusion hemisphere reaches, and how many rays it takes to get there.
//
// 0.45 m is chosen against the building rather than against a rule of thumb. A coffer in the dome
// is 0.225 m deep and between 0.309 and 1.004 m across, a flute is 0.12 m across, a niche is
// 0.675 m deep, a window reveal is a third of a metre: a radius under a quarter of a metre sees
// none of them as a recess, and one over half a metre starts closing whole rooms and doing the
// light grid's job badly. It is the scale that lies between the one voxel corner occlusion covers
// and the 0.4 m lattice the light grid samples, which is the entire reason this term exists.
//
// Thirty-two rays because the directions are FIXED -- see occlusion.hpp -- so the count buys
// smoothness in space and not noise, and sixteen shows the Fibonacci spiral on a large flat floor.
constexpr f64 kAoRadiusMetres = 0.45;
constexpr i32 kAoRays = 32;
// <<< ao

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
    u64 key = 0;        // what it was baked FROM; a match means the file is still right
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

// The header, and the number that says which header it is.
//
// Version 1 filled all 192 bytes exactly -- there was no spare room to put anything in. Version 2
// carries the cutter pool that turns the shapes view from a pile of overlapping primitives into
// the resolved solid, so the header grew to 208 and the version says so. A version 1 file in the
// cache beside a version 2 viewer is a real state and it must produce a clear error rather than a
// wrong picture: `reuse` refuses it and rebakes, and `web/js/format.js` throws on it.
// >>> paintexport
// Version 3 fills the last eight spare bytes of the header with a CHUNK DIRECTORY, so that anything
// added after this needs no more of the header and moves none of the blocks in front of it: a
// four-character name, an offset and a size, listed at the end of the file. The first two entries
// are `FLDG` and `PANT`, the field graph and the paint stack, which is what lets the ◉ view shade
// with the clip's own materials instead of one flat grey.
constexpr u32 kFormatVersion = 3;
// <<< paintexport
constexpr usize kHeaderBytes = 208;
// >>> chunkdir
constexpr usize kChunkOffsetAt = 200;
constexpr usize kChunkCountAt = 204;
constexpr usize kChunkEntryBytes = 16;
// <<< chunkdir
// 0 op, 4 cut_start, 8 scale, 12..59 the 3x4 placement, 60..91 eight parameters,
// 92..115 the world box, 116 cut_count. Matched by SHAPE_BYTES in web/js/format.js.
constexpr usize kShapeBytes = 120;
// Six RGBA32F texels: three matrix rows, eight parameters, then (op, scale, pad, pad). Twenty-two
// floats carry meaning and two are padding, because a texel is four.
constexpr usize kCutterFloats = 24;
constexpr usize kCutterBytes = kCutterFloats * sizeof(f32);


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

// >>> paintexport
// --------------------------------------------------------------------------------------
// The chunk directory
//
// Everything above this comment is at a fixed offset, and every one of those offsets moved the last
// time something was added — version 1 to version 2 cost a rebake of every clip in the cache and a
// hard error in the viewer, because there was no room left in the header to say where a new block
// was. There is room for exactly one more such change, and this is it spent well: eight bytes that
// point at a LIST of blocks, each with a name, so the next thing to be added costs an entry and
// moves nothing.
//
//   header 200   u32 chunkOffset, where the directory starts
//   header 204   u32 chunkCount
//   each entry   char fourcc[4]; u32 offset; u32 size; u32 reserved     -- 16 bytes
//
// Payloads are written before the directory and each is padded to sixteen bytes, so a reader may
// take a typed-array view straight onto one. The padding is not counted in `size`.
// --------------------------------------------------------------------------------------

// <<< paintexport
// >>> chunkdir
// FOUR agents wrote this mechanism independently, each told to write it "as if you are the first".
// That was right about the intent and wrong about the mechanics: two implementations of one thing
// do not merge, and all four were correct on their own. This is the reconciliation and there is
// now exactly one of it — the ambient-occlusion agent's writer, which is the one shaped to be
// PUSHED INTO rather than edited, with the sixteen-byte payload padding the format was specified
// with so a reader may take a typed-array view straight onto a payload. `web/js/format.js` holds
// the matching reader, `clip.chunk('FOUR')`, which is the irradiance agent's.
//
// Anybody adding a baked term pushes one `Chunk` here and reads it with one `clip.chunk()`. That
// is the whole contract, and it is the reason nothing after version 3 has to move a byte.
struct Chunk {
    char fourcc[4]{' ', ' ', ' ', ' '};
    std::vector<u8> bytes;
};

Chunk make_chunk(const char* name, std::vector<u8> bytes) {
    Chunk chunk;
    for (i32 i = 0; i < 4; ++i) chunk.fourcc[i] = name[i];
    chunk.bytes = std::move(bytes);
    return chunk;
}

// The directory first, then the payloads, then the two header words that point at the directory.
// Every payload starts on a sixteen-byte boundary; the padding is not counted in `size`.
void append_chunks(std::vector<u8>& out, const std::vector<Chunk>& chunks) {
    if (chunks.empty()) {
        put_u32(out, kChunkOffsetAt, 0);
        put_u32(out, kChunkCountAt, 0);
        return;
    }
    while ((out.size() & 15u) != 0) out.push_back(0);
    const usize directory = out.size();
    put_u32(out, kChunkOffsetAt, static_cast<u32>(directory));
    put_u32(out, kChunkCountAt, static_cast<u32>(chunks.size()));
    out.resize(directory + chunks.size() * kChunkEntryBytes, 0);
    for (usize i = 0; i < chunks.size(); ++i) {
        while ((out.size() & 15u) != 0) out.push_back(0);
        const usize at = directory + i * kChunkEntryBytes;
        for (i32 c = 0; c < 4; ++c) out[at + static_cast<usize>(c)] =
            static_cast<u8>(chunks[i].fourcc[c]);
        put_u32(out, at + 4, static_cast<u32>(out.size()));
        put_u32(out, at + 8, static_cast<u32>(chunks[i].bytes.size()));
        put_u32(out, at + 12, 0);
        out.insert(out.end(), chunks[i].bytes.begin(), chunks[i].bytes.end());
    }
}
// <<< chunkdir

// --------------------------------------------------------------------------------------
// The clip before it was voxels
//
// A clip is a description, and everything the viewer draws is what fell out of evaluating it. The
// description itself -- the boxes and cylinders somebody actually typed, the ones a later
// difference took away again, the shapes that are three millimetres apart and fight over a voxel
// -- is not in the mesh at all, and it is what you want when the voxels look wrong.
//
// So the field is walked from the solid and every SHAPE LEAF is written out with the transform
// that puts it where it is. The viewer ray-marches them, so what it draws is the true surface at
// whatever distance you look from: no voxels, no resolution.
//
// The walk carries three things down:
//
//   a 3x4 matrix   world -> this node's own space, which is exactly what `eval` does to the point
//                  as it descends, so it is accumulated rather than inverted
//   a scale        what `Op::Scale` multiplies a distance by, so a scaled shape marches safely
//   the cutters    every leaf of every `difference` subtrahend standing above this shape, in the
//                  same world space it is in
//
// `mirror` folds space rather than moving it, so it emits its child twice, once through the fold.
// `repeat` is expanded up to a cap. Ops that bend the space they contain -- twist, bend, revolve,
// displace -- have no honest affine placement for their children and are counted rather than
// drawn, and the viewer says how many were left out.
//
// # Local CSG by scope, which is what makes a hole a hole
//
// This used to flatten the tree into independent leaves, each with a `sign` of +1 or -1, and the
// viewer marched each one ALONE. So a doorway was not a hole: it was a red slab standing in front
// of the wall it was supposed to go through, and every overlap anybody had ever written was on
// screen as raw overlapping primitives. Reported as "make the sdf raw view mode be the processed
// sdfs already cut", and that is exactly the fault.
//
// The fix is not to evaluate the whole tree per march step -- the facility is 15,190 shapes and no
// phone will walk that at every step of every ray. Instead each leaf carries the SUBTRAHENDS THAT
// APPLY TO IT: on a `difference`, child 0 is walked with `inherited + every leaf of children 1..n`,
// and children 1..n are not emitted as shapes at all. That scope is what stops two unrelated
// shapes that merely happen to overlap in space from cutting each other, and it is small: the
// cutters are then filtered to the ones whose world box actually touches the leaf's, which for
// almost every leaf leaves none, one or two.
//
// The viewer does `d = max(d_self, -d_cutter)` per cutter at each step, which is exact subtraction.
//
// **The one place it is not exact, said plainly.** Flattening a subtrahend subtree to a list of
// leaves treats that subtree as a UNION of them. That is right when the subtrahend is a union,
// which is nearly always what a `difference` in a clip takes away, and it OVER-CUTS when the
// subtrahend is itself an `intersection` or another `difference`. Both are counted while baking
// and reported per clip, so a wrong picture has a number next to it rather than being silent.
// --------------------------------------------------------------------------------------

// A leaf, and the slice of the cutter pool that is subtracted from it. `cut_start` and `cut_count`
// index `ShapeWalk::pool`, which the viewer uploads as one float texture.
struct Shape {
    u32 op = 0;
    u32 cut_start = 0;
    f32 scale = 1.0f;     // what a distance in this shape's space is worth in metres
    f32 world_to_local[12]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    f32 a[8]{};
    f32 low[3]{0, 0, 0};
    f32 high[3]{0, 0, 0};
    u32 cut_count = 0;
};

// One thing taken away, in world space. The box is the baker's own -- it is what decides whether
// this cutter is in range of a given leaf -- and is not written to the file; the viewer only ever
// needs enough to evaluate the distance.
struct Cutter {
    u32 op = 0;
    f32 scale = 1.0f;
    f32 world_to_local[12]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    f32 a[8]{};
    f32 low[3]{0, 0, 0};
    f32 high[3]{0, 0, 0};
};

struct Placement {
    f64 m[12]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};   // rows of [L | t], world -> local
    f64 scale = 1.0;
};

// A(p) = L p + t, then whatever the node does to the result.
Placement after_translate(const Placement& in, f64 x, f64 y, f64 z) {
    Placement out = in;
    out.m[3] -= x;
    out.m[7] -= y;
    out.m[11] -= z;
    return out;
}

Placement after_linear(const Placement& in, const f64 r[9]) {
    Placement out = in;
    for (i32 row = 0; row < 3; ++row) {
        for (i32 col = 0; col < 4; ++col) {
            out.m[row * 4 + col] = r[row * 3 + 0] * in.m[0 * 4 + col] +
                                   r[row * 3 + 1] * in.m[1 * 4 + col] +
                                   r[row * 3 + 2] * in.m[2 * 4 + col];
        }
    }
    return out;
}

// The viewer's own numbering, and it is deliberately not the enum's.
//
// `src/forge/field.hpp` comes from whichever branch is being baked, and `Op` is a plain enum whose
// values shift the moment anybody inserts a shape into the middle of the list -- which is exactly
// what the branch that added `arc` did. A format that shipped the enum's numbers would draw
// cylinders as capsules the first time somebody added a solid, and nothing would say so.
constexpr i32 kShapeUnknown = -1;

i32 web_op(ws::forge::Op op) {
    using ws::forge::Op;
    switch (op) {
        case Op::Sphere: return 0;
        case Op::Box: return 1;
        case Op::Cylinder: return 2;
        case Op::Capsule: return 3;
        case Op::Torus: return 4;
        case Op::Cone: return 5;
        case Op::Plane: return 6;
        case Op::Ellipsoid: return 7;
        default: return kShapeUnknown;
    }
}

bool is_shape_leaf(ws::forge::Op op) { return web_op(op) != kShapeUnknown; }

// >>> paintexport
// The paint field graph numbers these eight solids exactly as `web_op` does, so one GLSL `sdf()`
// serves the shapes view and the paint field both. Two tables that must agree and nothing checking
// that they do is how a cylinder gets drawn as a capsule, so this is asked once at startup and the
// bake refuses rather than writing a file whose two halves disagree.
bool op_numbering_agrees() {
    using ws::forge::Op;
    const Op solids[8] = {Op::Sphere, Op::Box,   Op::Cylinder, Op::Capsule,
                          Op::Torus,  Op::Cone,  Op::Plane,    Op::Ellipsoid};
    for (const Op op : solids) {
        if (static_cast<u32>(web_op(op)) != ws::bake::field_op(op)) return false;
    }
    return true;
}
// <<< paintexport

struct ShapeWalk {
    const ws::forge::Field* field = nullptr;
    std::string clip;
    std::vector<Shape> shapes;
    std::vector<f32> pool;                              // kCutterFloats per cutter
    std::unordered_map<std::string, u32> pool_runs;     // a run of cutters -> where it already is
    f64 low[3]{-1e9, -1e9, -1e9};
    f64 high[3]{1e9, 1e9, 1e9};
    usize skipped = 0;             // subtrees under an op with no affine placement
    usize capped = 0;              // instances a repeat would have made past the cap
    usize cutters_skipped = 0;     // the same, inside a subtrahend: a hole that came out too small
    usize over_cap = 0;            // shapes wanting more cutters than one shape may have
    usize over_cap_worst = 0;      // and the most any one of them wanted
    usize pool_full = 0;           // cutters dropped because the pool itself filled
    usize sub_intersections = 0;   // subtrahend subtrees holding an intersection -- over-cuts
    usize sub_differences = 0;     // subtrahend subtrees holding a difference   -- over-cuts
    usize warned = 0;
    static constexpr usize kMaxShapes = 20000;
    static constexpr i32 kMaxRepeat = 24;
    // Sixteen is what one shape may subtract. A wall with more windows than that loses the
    // smallest of them and says so rather than quietly drawing a wall with no window in it.
    static constexpr usize kMaxCutters = 16;
    // And a ceiling on the whole pool, because it becomes a texture on a phone. 65,536 cutters is
    // 6 MB of RGBA32F.
    static constexpr usize kMaxPool = 65536;

    usize cutter_count() const { return pool.size() / kCutterFloats; }
};

// Where a leaf lands in world space, so the viewer has a box to march inside and the baker has one
// to test cutters against. The field knows the shape's box in its OWN space; the corners of that
// box come back out through the placement, which is inverted here, once, for eight corners.
enum class BoxResult { Ok, Singular, Outside };

BoxResult leaf_world_box(const ShapeWalk& walk, u32 node, const Placement& at, f32 low[3],
                         f32 high[3]) {
    const ws::forge::Field::Aabb local = walk.field->bounds_of(node);
    if (local.infinite()) {
        for (i32 axis = 0; axis < 3; ++axis) {
            low[axis] = static_cast<f32>(walk.low[axis]);
            high[axis] = static_cast<f32>(walk.high[axis]);
        }
        return BoxResult::Ok;
    }

    f64 inv[12];
    const f64 det = at.m[0] * (at.m[5] * at.m[10] - at.m[6] * at.m[9]) -
                    at.m[1] * (at.m[4] * at.m[10] - at.m[6] * at.m[8]) +
                    at.m[2] * (at.m[4] * at.m[9] - at.m[5] * at.m[8]);
    if (std::abs(det) < 1e-12) return BoxResult::Singular;
    const f64 s = 1.0 / det;
    inv[0] = (at.m[5] * at.m[10] - at.m[6] * at.m[9]) * s;
    inv[1] = (at.m[2] * at.m[9] - at.m[1] * at.m[10]) * s;
    inv[2] = (at.m[1] * at.m[6] - at.m[2] * at.m[5]) * s;
    inv[4] = (at.m[6] * at.m[8] - at.m[4] * at.m[10]) * s;
    inv[5] = (at.m[0] * at.m[10] - at.m[2] * at.m[8]) * s;
    inv[6] = (at.m[2] * at.m[4] - at.m[0] * at.m[6]) * s;
    inv[8] = (at.m[4] * at.m[9] - at.m[5] * at.m[8]) * s;
    inv[9] = (at.m[1] * at.m[8] - at.m[0] * at.m[9]) * s;
    inv[10] = (at.m[0] * at.m[5] - at.m[1] * at.m[4]) * s;
    const f64 tx = at.m[3], ty = at.m[7], tz = at.m[11];
    inv[3] = -(inv[0] * tx + inv[1] * ty + inv[2] * tz);
    inv[7] = -(inv[4] * tx + inv[5] * ty + inv[6] * tz);
    inv[11] = -(inv[8] * tx + inv[9] * ty + inv[10] * tz);

    f64 lo[3] = {1e30, 1e30, 1e30};
    f64 hi[3] = {-1e30, -1e30, -1e30};
    for (i32 corner = 0; corner < 8; ++corner) {
        const f64 c[3] = {(corner & 1) ? local.high.x : local.low.x,
                          (corner & 2) ? local.high.y : local.low.y,
                          (corner & 4) ? local.high.z : local.low.z};
        for (i32 axis = 0; axis < 3; ++axis) {
            const f64 v = inv[axis * 4 + 0] * c[0] + inv[axis * 4 + 1] * c[1] +
                          inv[axis * 4 + 2] * c[2] + inv[axis * 4 + 3];
            lo[axis] = std::min(lo[axis], v);
            hi[axis] = std::max(hi[axis], v);
        }
    }
    for (i32 axis = 0; axis < 3; ++axis) {
        low[axis] = static_cast<f32>(std::max(lo[axis] - 0.01, walk.low[axis]));
        high[axis] = static_cast<f32>(std::min(hi[axis] + 0.01, walk.high[axis]));
        if (low[axis] >= high[axis]) return BoxResult::Outside;
    }
    return BoxResult::Ok;
}

// What one `difference` takes away, flattened. See the header comment for where this is exact and
// where it over-cuts; `intersections` and `differences` are what say which of the two happened.
struct Subtrahend {
    std::vector<Cutter> list;
    usize skipped = 0;
    usize intersections = 0;
    usize differences = 0;
};

void collect_cutters(ShapeWalk& walk, Subtrahend& out, u32 node, const Placement& at, i32 depth);

void walk_shapes(ShapeWalk& walk, u32 node, const Placement& at, i32 depth,
                 const std::vector<Cutter>& inherited);

// Do two world boxes touch? Cutters that do not are not this leaf's business, and dropping them is
// what keeps the per-shape list at nothing or one or two on a real clip.
bool boxes_overlap(const f32 a_low[3], const f32 a_high[3], const f32 b_low[3],
                   const f32 b_high[3]) {
    for (i32 axis = 0; axis < 3; ++axis) {
        if (a_high[axis] < b_low[axis] || b_high[axis] < a_low[axis]) return false;
    }
    return true;
}

f64 overlap_volume(const f32 a_low[3], const f32 a_high[3], const f32 b_low[3],
                   const f32 b_high[3]) {
    f64 volume = 1.0;
    for (i32 axis = 0; axis < 3; ++axis) {
        const f64 span = std::min(a_high[axis], b_high[axis]) - std::max(a_low[axis], b_low[axis]);
        volume *= std::max(span, 0.0);
    }
    return volume;
}

// A run of cutters into the pool, deduplicated: every leaf of one wall shares that wall's windows,
// so the same run is written once and pointed at many times.
void place_run(ShapeWalk& walk, const std::vector<Cutter>& run, Shape& shape) {
    if (run.empty()) return;
    std::string key;
    key.reserve(run.size() * kCutterFloats * sizeof(f32));
    std::vector<f32> payload;
    payload.reserve(run.size() * kCutterFloats);
    for (const Cutter& cutter : run) {
        f32 record[kCutterFloats]{};
        for (i32 i = 0; i < 12; ++i) record[i] = cutter.world_to_local[i];
        for (i32 i = 0; i < 8; ++i) record[12 + i] = cutter.a[i];
        record[20] = static_cast<f32>(cutter.op);
        record[21] = cutter.scale;
        payload.insert(payload.end(), record, record + kCutterFloats);
    }
    key.assign(reinterpret_cast<const char*>(payload.data()), payload.size() * sizeof(f32));

    const auto found = walk.pool_runs.find(key);
    if (found != walk.pool_runs.end()) {
        shape.cut_start = found->second;
        shape.cut_count = static_cast<u32>(run.size());
        return;
    }
    if (walk.cutter_count() + run.size() > ShapeWalk::kMaxPool) {
        walk.pool_full += run.size();
        return;   // no cutters rather than the wrong ones; the count says it happened
    }
    shape.cut_start = static_cast<u32>(walk.cutter_count());
    shape.cut_count = static_cast<u32>(run.size());
    walk.pool.insert(walk.pool.end(), payload.begin(), payload.end());
    walk.pool_runs.emplace(std::move(key), shape.cut_start);
}

void walk_shapes(ShapeWalk& walk, u32 node, const Placement& at, i32 depth,
                 const std::vector<Cutter>& inherited) {
    using ws::forge::Op;
    if (depth > 64 || walk.shapes.size() >= ShapeWalk::kMaxShapes) return;
    const ws::forge::Node& n = walk.field->node(node);
    const f64* a = n.a;

    if (is_shape_leaf(n.op)) {
        Shape shape;
        shape.op = static_cast<u32>(web_op(n.op));
        shape.scale = static_cast<f32>(at.scale);
        for (i32 i = 0; i < 12; ++i) shape.world_to_local[i] = static_cast<f32>(at.m[i]);
        for (i32 i = 0; i < 8; ++i) shape.a[i] = static_cast<f32>(a[i]);

        const BoxResult box = leaf_world_box(walk, node, at, shape.low, shape.high);
        if (box == BoxResult::Singular) {
            walk.skipped += 1;
            return;
        }
        if (box == BoxResult::Outside) return;   // entirely outside the clip

        // Only the subtrahends that actually reach this leaf. Over the cap, the biggest overlaps
        // are kept -- a wall keeps its doorway and loses a corner bead, rather than the other way
        // round -- and the count is warned about, because a silent truncation reads as "it worked".
        std::vector<Cutter> mine;
        for (const Cutter& cutter : inherited) {
            if (boxes_overlap(shape.low, shape.high, cutter.low, cutter.high)) mine.push_back(cutter);
        }
        if (mine.size() > ShapeWalk::kMaxCutters) {
            walk.over_cap += 1;
            walk.over_cap_worst = std::max(walk.over_cap_worst, mine.size());
            if (walk.warned < 4) {
                walk.warned += 1;
                // std::format, not printf: this logger takes {} and a "%zu" reaches the console
                // verbatim, which is a warning that says nothing.
                WS_LOG_WARN("bake_web", "{}: a shape is cut by {} shapes and the cap is {}",
                            walk.clip, mine.size(), ShapeWalk::kMaxCutters);
            }
            std::stable_sort(mine.begin(), mine.end(),
                             [&shape](const Cutter& x, const Cutter& y) {
                                 return overlap_volume(shape.low, shape.high, x.low, x.high) >
                                        overlap_volume(shape.low, shape.high, y.low, y.high);
                             });
            mine.resize(ShapeWalk::kMaxCutters);
        }
        place_run(walk, mine, shape);
        walk.shapes.push_back(shape);
        return;
    }

    switch (n.op) {
        case Op::Union:
        case Op::SmoothUnion:
        case Op::Intersection:
        case Op::SmoothIntersection:
        case Op::Min:
        case Op::Max:
            for (u32 c = 0; c < n.children; ++c) {
                walk_shapes(walk, n.child[c], at, depth + 1, inherited);
            }
            return;

        case Op::Difference:
        case Op::SmoothDifference: {
            // The subtrahends become CUTTERS on everything under child 0, and are not shapes of
            // their own. That is the whole of the fix: a hole is a hole and nothing is drawn red.
            Subtrahend sub;
            for (u32 c = 1; c < n.children; ++c) {
                collect_cutters(walk, sub, n.child[c], at, depth + 1);
            }
            walk.cutters_skipped += sub.skipped;
            if (sub.intersections > 0) walk.sub_intersections += 1;
            if (sub.differences > 0) walk.sub_differences += 1;

            if (n.children == 0) return;
            if (sub.list.empty()) {
                walk_shapes(walk, n.child[0], at, depth + 1, inherited);
                return;
            }
            std::vector<Cutter> scope = inherited;
            scope.insert(scope.end(), sub.list.begin(), sub.list.end());
            walk_shapes(walk, n.child[0], at, depth + 1, scope);
            return;
        }

        case Op::Translate:
            walk_shapes(walk, n.child[0], after_translate(at, a[0], a[1], a[2]), depth + 1,
                        inherited);
            return;

        case Op::Rotate: {
            // `eval` turns the POINT by the negated angles, x then y then z, in turns. The same
            // matrix is accumulated here, which is why nothing has to be inverted.
            const f64 tau = 6.283185307179586;
            const f64 cx = std::cos(-a[0] * tau), sx = std::sin(-a[0] * tau);
            const f64 cy = std::cos(-a[1] * tau), sy = std::sin(-a[1] * tau);
            const f64 cz = std::cos(-a[2] * tau), sz = std::sin(-a[2] * tau);
            const f64 rx[9] = {1, 0, 0, 0, cx, -sx, 0, sx, cx};
            const f64 ry[9] = {cy, 0, sy, 0, 1, 0, -sy, 0, cy};
            const f64 rz[9] = {cz, -sz, 0, sz, cz, 0, 0, 0, 1};
            walk_shapes(walk, n.child[0],
                        after_linear(after_linear(after_linear(at, rx), ry), rz), depth + 1,
                        inherited);
            return;
        }

        case Op::Scale: {
            const f64 sx = (a[0] != 0.0) ? a[0] : 1.0;
            const f64 sy = (a[1] != 0.0) ? a[1] : 1.0;
            const f64 sz = (a[2] != 0.0) ? a[2] : 1.0;
            const f64 diag[9] = {1.0 / sx, 0, 0, 0, 1.0 / sy, 0, 0, 0, 1.0 / sz};
            Placement out = after_linear(at, diag);
            out.scale = at.scale * std::min(std::abs(sx), std::min(std::abs(sy), std::abs(sz)));
            walk_shapes(walk, n.child[0], out, depth + 1, inherited);
            return;
        }

        case Op::Mirror: {
            // A fold, not a move: the child is on both sides of the plane, so it is emitted twice.
            const u32 axis = static_cast<u32>(a[0]) % 3u;
            f64 flip[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
            flip[axis * 3 + axis] = -1.0;
            walk_shapes(walk, n.child[0], at, depth + 1, inherited);
            walk_shapes(walk, n.child[0], after_linear(at, flip), depth + 1, inherited);
            return;
        }

        case Op::Repeat: {
            const f64 period[3] = {a[0], a[1], a[2]};
            const f64 limit[3] = {a[3], a[4], a[5]};
            i32 count[3] = {1, 1, 1};
            for (i32 axis = 0; axis < 3; ++axis) {
                if (period[axis] > 1e-9) {
                    count[axis] = (limit[axis] > 0.0)
                                      ? static_cast<i32>(limit[axis]) * 2 + 1
                                      : ShapeWalk::kMaxRepeat;
                    count[axis] = std::min(count[axis], ShapeWalk::kMaxRepeat);
                }
            }
            for (i32 z = 0; z < count[2]; ++z) {
                for (i32 y = 0; y < count[1]; ++y) {
                    for (i32 x = 0; x < count[0]; ++x) {
                        const f64 step[3] = {
                            (count[0] > 1) ? (x - count[0] / 2) * period[0] : 0.0,
                            (count[1] > 1) ? (y - count[1] / 2) * period[1] : 0.0,
                            (count[2] > 1) ? (z - count[2] / 2) * period[2] : 0.0};
                        walk_shapes(walk, n.child[0],
                                    after_translate(at, step[0], step[1], step[2]), depth + 1,
                                    inherited);
                    }
                }
            }
            return;
        }

        case Op::Shell:
        case Op::Round:
        case Op::Offset:
        case Op::Displace:
            // These change the SURFACE without moving the space its children live in, so the shapes
            // under them are exactly where they were. Displace is the one that matters: the whole
            // facility is `displace { furnished grain_fine }`, so skipping it returned nothing at
            // all for the one clip anybody opens first. Its second child is the pattern, which is
            // not a shape and is not descended into.
            walk_shapes(walk, n.child[0], at, depth + 1, inherited);
            return;

        default:
            // Everything else either bends the space its children live in, or is a pattern rather
            // than a shape. Counted, and the viewer says so.
            if (n.children > 0) walk.skipped += 1;
            return;
    }
}

// The same descent, but everything it reaches becomes something TAKEN AWAY rather than something
// drawn. It is deliberately the same set of ops with the same placement arithmetic, so a cutter
// lands exactly where the shape it mirrors would have.
void collect_cutters(ShapeWalk& walk, Subtrahend& out, u32 node, const Placement& at, i32 depth) {
    using ws::forge::Op;
    // A subtrahend past the per-shape cap can be stopped here: nothing beyond it can be kept.
    if (depth > 64 || out.list.size() >= ShapeWalk::kMaxCutters * 4) return;
    const ws::forge::Node& n = walk.field->node(node);
    const f64* a = n.a;

    if (is_shape_leaf(n.op)) {
        Cutter cutter;
        cutter.op = static_cast<u32>(web_op(n.op));
        cutter.scale = static_cast<f32>(at.scale);
        for (i32 i = 0; i < 12; ++i) cutter.world_to_local[i] = static_cast<f32>(at.m[i]);
        for (i32 i = 0; i < 8; ++i) cutter.a[i] = static_cast<f32>(a[i]);
        const BoxResult box = leaf_world_box(walk, node, at, cutter.low, cutter.high);
        if (box == BoxResult::Singular) {
            out.skipped += 1;
            return;
        }
        if (box == BoxResult::Outside) return;   // cuts nothing inside the clip
        out.list.push_back(cutter);
        return;
    }

    switch (n.op) {
        case Op::Union:
        case Op::SmoothUnion:
        case Op::Min:
            for (u32 c = 0; c < n.children; ++c) collect_cutters(walk, out, n.child[c], at, depth + 1);
            return;

        case Op::Intersection:
        case Op::SmoothIntersection:
        case Op::Max:
            // The honest limitation. A union of these leaves is bigger than their intersection, so
            // taking it away removes more than the clip does. Counted, and written down in
            // documentation/24-clip-viewer.md rather than hidden.
            out.intersections += 1;
            for (u32 c = 0; c < n.children; ++c) collect_cutters(walk, out, n.child[c], at, depth + 1);
            return;

        case Op::Difference:
        case Op::SmoothDifference:
            // A hole inside a hole. Same family, same over-cut: the subtrahend's own subtrahend
            // should be putting matter back and instead joins the union that takes it away.
            out.differences += 1;
            for (u32 c = 0; c < n.children; ++c) collect_cutters(walk, out, n.child[c], at, depth + 1);
            return;

        case Op::Translate:
            collect_cutters(walk, out, n.child[0], after_translate(at, a[0], a[1], a[2]), depth + 1);
            return;

        case Op::Rotate: {
            const f64 tau = 6.283185307179586;
            const f64 cx = std::cos(-a[0] * tau), sx = std::sin(-a[0] * tau);
            const f64 cy = std::cos(-a[1] * tau), sy = std::sin(-a[1] * tau);
            const f64 cz = std::cos(-a[2] * tau), sz = std::sin(-a[2] * tau);
            const f64 rx[9] = {1, 0, 0, 0, cx, -sx, 0, sx, cx};
            const f64 ry[9] = {cy, 0, sy, 0, 1, 0, -sy, 0, cy};
            const f64 rz[9] = {cz, -sz, 0, sz, cz, 0, 0, 0, 1};
            collect_cutters(walk, out, n.child[0],
                            after_linear(after_linear(after_linear(at, rx), ry), rz), depth + 1);
            return;
        }

        case Op::Scale: {
            const f64 sx = (a[0] != 0.0) ? a[0] : 1.0;
            const f64 sy = (a[1] != 0.0) ? a[1] : 1.0;
            const f64 sz = (a[2] != 0.0) ? a[2] : 1.0;
            const f64 diag[9] = {1.0 / sx, 0, 0, 0, 1.0 / sy, 0, 0, 0, 1.0 / sz};
            Placement scaled = after_linear(at, diag);
            scaled.scale = at.scale * std::min(std::abs(sx), std::min(std::abs(sy), std::abs(sz)));
            collect_cutters(walk, out, n.child[0], scaled, depth + 1);
            return;
        }

        case Op::Mirror: {
            const u32 axis = static_cast<u32>(a[0]) % 3u;
            f64 flip[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
            flip[axis * 3 + axis] = -1.0;
            collect_cutters(walk, out, n.child[0], at, depth + 1);
            collect_cutters(walk, out, n.child[0], after_linear(at, flip), depth + 1);
            return;
        }

        case Op::Repeat: {
            const f64 period[3] = {a[0], a[1], a[2]};
            const f64 limit[3] = {a[3], a[4], a[5]};
            i32 count[3] = {1, 1, 1};
            for (i32 axis = 0; axis < 3; ++axis) {
                if (period[axis] > 1e-9) {
                    count[axis] = (limit[axis] > 0.0)
                                      ? static_cast<i32>(limit[axis]) * 2 + 1
                                      : ShapeWalk::kMaxRepeat;
                    count[axis] = std::min(count[axis], ShapeWalk::kMaxRepeat);
                }
            }
            for (i32 z = 0; z < count[2]; ++z) {
                for (i32 y = 0; y < count[1]; ++y) {
                    for (i32 x = 0; x < count[0]; ++x) {
                        const f64 step[3] = {
                            (count[0] > 1) ? (x - count[0] / 2) * period[0] : 0.0,
                            (count[1] > 1) ? (y - count[1] / 2) * period[1] : 0.0,
                            (count[2] > 1) ? (z - count[2] / 2) * period[2] : 0.0};
                        collect_cutters(walk, out, n.child[0],
                                        after_translate(at, step[0], step[1], step[2]), depth + 1);
                    }
                }
            }
            return;
        }

        case Op::Shell:
        case Op::Round:
        case Op::Offset:
        case Op::Displace:
            collect_cutters(walk, out, n.child[0], at, depth + 1);
            return;

        default:
            // A subtrahend nobody can place: the hole it should have cut is simply not cut, so this
            // is counted separately from the shapes that go missing for the same reason.
            if (n.children > 0) out.skipped += 1;
            return;
    }
}

// --------------------------------------------------------------------------------------
// Not baking it again
//
// Almost nothing changes between two runs. An agent edits one fragment and the other sixty-two
// clips are byte for byte what they were, but each was being sampled from scratch anyway -- half
// an hour of a runner to rebuild a building that had not moved, every time anybody pushed.
//
// So each file carries the key of what produced it, and a clip whose key still matches is read
// back rather than rebuilt. Everything the index needs is already in the header, which is why this
// needs no sidecar and no JSON to parse: the file IS the record of itself.
//
// The key covers the spliced source (so an edit to any included fragment invalidates it), the
// resolution settings, and a hash of the sampler's own code. Miss any of those and the site serves
// something stale with a current-looking hash on it, which is the worst failure this can have --
// so when in doubt the key changes and the clip is rebaked.
// --------------------------------------------------------------------------------------

u64 read_u32(const std::vector<u8>& bytes, usize at) {
    return static_cast<u64>(bytes[at]) | (static_cast<u64>(bytes[at + 1]) << 8) |
           (static_cast<u64>(bytes[at + 2]) << 16) | (static_cast<u64>(bytes[at + 3]) << 24);
}

f32 read_f32(const std::vector<u8>& bytes, usize at) {
    const u32 raw = static_cast<u32>(read_u32(bytes, at));
    f32 value = 0.0f;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

bool reuse(const Options& options, const fs::path& relative, u64 key, Baked& baked) {
    if (options.force) return false;
    const std::string id = identifier(relative);
    const fs::path file = options.out / (id + ".wsc");

    std::ifstream stream(file, std::ios::binary | std::ios::ate);
    if (!stream) return false;
    const std::streamsize size = stream.tellg();
    if (size < static_cast<std::streamsize>(kHeaderBytes)) return false;
    stream.seekg(0);
    std::vector<u8> bytes(static_cast<usize>(size));
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), size)) return false;

    if (bytes[0] != 'W' || bytes[1] != 'S' || bytes[2] != 'C' || bytes[3] != 'V') return false;
    // A file from an older format is not stale-but-usable, it is unreadable: the offsets moved.
    // Refusing it here is what makes a cached web/data from before the cutter pool rebake itself
    // instead of being served to a viewer that would throw on it.
    if (read_u32(bytes, 4) != kFormatVersion) return false;
    const u64 stored = read_u32(bytes, 180) | (read_u32(bytes, 184) << 32);
    if (stored != key) return false;

    baked.id = id;
    baked.source = relative.generic_string();
    const fs::path parent = relative.parent_path();
    baked.group = parent.empty() ? std::string("clips") : parent.generic_string();
    for (i32 axis = 0; axis < 3; ++axis) {
        baked.dims[axis] = static_cast<i32>(read_u32(bytes, 8 + static_cast<usize>(axis) * 4));
        baked.origin[axis] = read_f32(bytes, 24 + static_cast<usize>(axis) * 4);
        baked.matter_low[axis] = read_f32(bytes, 148 + static_cast<usize>(axis) * 4);
        baked.matter_high[axis] = read_f32(bytes, 160 + static_cast<usize>(axis) * 4);
    }
    baked.metre = static_cast<i32>(read_u32(bytes, 20));
    baked.authored_metre = static_cast<i32>(read_u32(bytes, 172));
    baked.solid = read_u32(bytes, 176);
    baked.quads = static_cast<u32>(read_u32(bytes, 52) + read_u32(bytes, 56));
    baked.key = key;
    baked.hash = fnv1a(bytes.data(), bytes.size());
    baked.bytes = bytes.size();

    std::printf("  unchanged  %d/m  %u quads  %.1f MB\n", baked.metre, baked.quads,
                static_cast<f64>(bytes.size()) / (1024.0 * 1024.0));
    return true;
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
    // `baked.key` is set by the caller before this runs; it is written into the header below.
    ws::forge::Script& script = program.script;
    ws::VoxelTypeTable& types = program.types;

    ws::forge::SampleSettings settings = script.settings;
    baked.authored_metre = settings.voxels_per_metre;
    u32 shapes_from = root;

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
        // The shapes view wants the FRAGMENT'S shapes, and this is the last moment they can be
        // told apart: after the intersection below the root is `intersection { part, the whole
        // building }`, and a walk of that descends into both. The portico came out with 15,927
        // shapes against the building's own 15,190 -- it was showing the entire facility.
        shapes_from = root;
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
    const i32 cap = (is_part && options.part_metre > 0)
                        ? std::min(options.max_metre, options.part_metre)
                        : options.max_metre;
    i32 metre = std::min(cap, settings.voxels_per_metre);
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

    // >>> matvol
    // What the stone inside a wall is made of, and how far a ray travels through it.
    //
    // On the collision grid's own cells, so the viewer already has the pitch; built HERE, before
    // the header is written, because asking for a voxel's material interns it and a stone that
    // only ever exists inside a wall has no palette index until this asks for one. Building it
    // after `put_u32(out, 48, palette.size())` would write a material count the material block
    // then disagrees with. See tools/bake/matvol.hpp.
    const u64 matvol_began = ws::now_ns();
    const ws::bake::matvol::Volume volume = ws::bake::matvol::build(
        clip.size, metre, collision.voxels_per_metre, mesher.palette(),
        [&mesher](i32 x, i32 y, i32 z) { return mesher.solid(x, y, z); },
        [&mesher](i32 x, i32 y, i32 z) {
            const ws::VoxelTypeId type = mesher.type_at(x, y, z);
            if (type == ws::kAir) return -1;
            return static_cast<i32>(mesher.material_of(type));
        });
    const f64 matvol_seconds = static_cast<f64>(ws::now_ns() - matvol_began) / 1e9;
    if (volume.dropped_materials > 0) {
        WS_LOG_WARN("bake_web", "{}: {} materials past the material volume's {}, {} cells painted "
                                "as the nearest kept stone",
                    relative.generic_string(), volume.dropped_materials,
                    ws::bake::matvol::kMaxPalette, volume.remapped_cells);
    }
    // <<< matvol

    const f64 origin[3] = {settings.low.x, settings.low.y, settings.low.z};
    const f64 size_metres[3] = {static_cast<f64>(clip.size[0]) / static_cast<f64>(metre),
                                static_cast<f64>(clip.size[1]) / static_cast<f64>(metre),
                                static_cast<f64>(clip.size[2]) / static_cast<f64>(metre)};
    const LightGrid light = bake_light(coarse, origin, size_metres, jobs);

    // >>> gi
    // ...and the light that has BOUNCED, with the colour it bounced off. The grid above is a
    // visibility term and cannot carry colour; this one is six RGB values a point at half its
    // pitch, gathered against the same coarse copy of the clip, iterated twice so a wall lit by a
    // floor lit by the sun is lit. tools/bake/irradiance.hpp is the whole of it.
    ws::bakeweb::IrradianceSettings gi_settings;
    for (i32 axis = 0; axis < 3; ++axis) {
        gi_settings.origin[axis] = origin[axis];
        gi_settings.size_metres[axis] = size_metres[axis];
    }
    {
        const f64 length = std::sqrt(kSunDir[0] * kSunDir[0] + kSunDir[1] * kSunDir[1] +
                                     kSunDir[2] * kSunDir[2]);
        for (i32 axis = 0; axis < 3; ++axis) gi_settings.sun[axis] = kSunDir[axis] / length;
    }
    gi_settings.cell = light.cell * 2.0;
    ws::bakeweb::IrradianceStats gi_stats;
    const u64 gi_began = ws::now_ns();
    const ws::bakeweb::IrradianceVolume gi = ws::bakeweb::bake_irradiance(
        clip, types, coarse, light.texels, light.dims, light.cell, metre, gi_settings, jobs,
        &gi_stats);
    gi_stats.seconds = static_cast<f64>(ws::now_ns() - gi_began) / 1e9;
    // <<< gi
    // >>> ao
    // Ambient occlusion, one texel per exposed voxel face.
    //
    // The quads are handed over in EXACTLY the order they are written to the file below -- every
    // opaque face group in face order, then every transparent one -- because that order is the
    // allocation. The viewer re-derives where each quad's run starts by prefix-summing `w * h`
    // over the quads it already has, so the file carries no per-quad offset; the total is written
    // down and checked at load, so the two derivations either agree or one of them says so.
    //
    // tools/bake/occlusion.hpp is what this is, why it is an atlas rather than a volume, and why
    // it is a different term from the corner occlusion in the quad record, which it does not
    // touch.
    std::vector<ws::bake::AoQuad> ao_quads;
    for (i32 pass = 0; pass < 2; ++pass) {
        const std::array<std::vector<Quad>, 6>& lists =
            (pass == 0) ? mesher.opaque() : mesher.transparent();
        for (i32 face = 0; face < 6; ++face) {
            for (const Quad& q : lists[static_cast<usize>(face)]) {
                ao_quads.push_back(ws::bake::AoQuad{static_cast<i32>(q.x), static_cast<i32>(q.y),
                                                    static_cast<i32>(q.z), static_cast<i32>(q.w),
                                                    static_cast<i32>(q.h), face});
            }
        }
    }
    const u64 ao_began = ws::now_ns();
    const ws::bake::Occlusion occlusion = ws::bake::bake_occlusion(
        [&mesher](i32 x, i32 y, i32 z) { return mesher.solid(x, y, z); }, ao_quads, metre,
        kAoRadiusMetres, kAoRays, jobs);
    const f64 ao_seconds = static_cast<f64>(ws::now_ns() - ao_began) / 1e9;
    // <<< ao
    // >>> probes
    // Reflection probes, cast against the same two grids the light was cast against. See
    // tools/bake/probes.hpp for where they go and why they are octahedral; this is only the wiring.
    ws::web::ProbeSet probes;
    {
        ws::web::ProbeInput input;
        input.clip = &clip;
        input.types = &types;
        // The CONSERVATIVE grid, not the coarse one the light uses. A `mirror` in these clips is a
        // coat of paint one voxel thick, and the light grid's copy only fills a cell when a third
        // of it is solid -- a reflection ray would walk straight through the wall it is supposed to
        // be showing.
        input.occupancy.bits = collision.bits.data();
        input.occupancy.dims[0] = collision.dims[0];
        input.occupancy.dims[1] = collision.dims[1];
        input.occupancy.dims[2] = collision.dims[2];
        input.occupancy.voxels_per_metre = collision.voxels_per_metre;
        input.light.texels = light.texels.data();
        input.light.dims[0] = light.dims[0];
        input.light.dims[1] = light.dims[1];
        input.light.dims[2] = light.dims[2];
        input.light.cell = light.cell;
        input.metre = metre;
        const f64 sun_length = std::sqrt(kSunDir[0] * kSunDir[0] + kSunDir[1] * kSunDir[1] +
                                         kSunDir[2] * kSunDir[2]);
        for (i32 axis = 0; axis < 3; ++axis) {
            input.origin[axis] = origin[axis];
            input.size_metres[axis] = size_metres[axis];
            input.sun[axis] = kSunDir[axis] / sun_length;
        }
        probes = ws::web::bake_probes(input, jobs);
    }
    const std::vector<u8> probe_bytes = ws::web::probe_chunk(probes);
    // <<< probes
    // >>> lights
    // The emitters, as LIGHTS rather than as bright paint: where each fitting is, how big, what
    // colour, how strong, and a baked cube of visibility per light so a sconce in one hall does not
    // light the hall next door. tools/bake/lights.hpp is the whole of it.
    const ws::web::LightBake lights = ws::web::bake_lights(clip, types, origin, metre, jobs);
    // <<< lights

    // And the clip as it was written, before any of the above.
    ShapeWalk walk;
    walk.field = &script.field;
    walk.clip = relative.generic_string();
    // Nothing may claim a box bigger than the clip. A `plane` is a half space, its bounds are
    // everywhere, and a two-billion-metre impostor flattens the depth buffer for everything else
    // on screen -- there are sixty-five planes in the facility.
    for (i32 axis = 0; axis < 3; ++axis) {
        walk.low[axis] = origin[axis] - 0.05;
        walk.high[axis] = origin[axis] + size_metres[axis] + 0.05;
    }
    walk_shapes(walk, shapes_from, Placement{}, 0, {});

    // >>> paintexport
    // ---- the paint stack, so the shapes above can be shaded ------------------------------------
    //
    // A shape has no material in this language: colour comes from the rules, in order, each painting
    // over the last, and every one of them is a field asked at the point. The marcher has the true
    // surface point, so it can run the same stack -- given the rules and the fields they need.
    //
    // Three things are decided here and each of them is a way of getting this wrong.
    //
    // **The space.** `apply_origin` translates every rule's `test` and `bake_root` gives a part the
    // same shift before `shapes_from` is walked, so the rules and the shapes are already in one
    // space and nothing is done to them. What `apply_origin` does NOT move is `rule.place`, and the
    // box derived from it is shifted in `rule_region` -- see the header of tools/bake/paint.hpp.
    //
    // **The region.** A rule is pruned only when its own box misses everywhere the viewer could put
    // a hit point, which is the union of the boxes the shapes are marched inside. Not the sampled
    // box: a fragment is baked out of the whole building's parse and carries the whole building's
    // paint stack, weathering coats and all, so most of that stack is about somewhere else entirely.
    //
    // **The materials.** A rule names a `VoxelTypeId`, and the file's material table is interned by
    // what a material LOOKS like. `material_of` is the mesher's own interning, so a rule and a quad
    // that name the same matter name the same record -- and a material that no voxel happens to
    // carry is appended to the table here rather than being missing from it, which costs sixteen
    // bytes and is the difference between a rule that can be drawn and one that cannot.
    f64 shape_low[3] = {walk.low[0], walk.low[1], walk.low[2]};
    f64 shape_high[3] = {walk.high[0], walk.high[1], walk.high[2]};
    if (!walk.shapes.empty()) {
        for (i32 axis = 0; axis < 3; ++axis) {
            shape_low[axis] = 1e30;
            shape_high[axis] = -1e30;
        }
        for (const Shape& shape : walk.shapes) {
            for (i32 axis = 0; axis < 3; ++axis) {
                shape_low[axis] = std::min(shape_low[axis], static_cast<f64>(shape.low[axis]));
                shape_high[axis] = std::max(shape_high[axis], static_cast<f64>(shape.high[axis]));
            }
        }
    }
    f64 paint_amplitude = 0.0;
    (void)script.field.undisplaced(root, paint_amplitude);
    // Every material in the palette at this moment is one some VOXEL carries, because the mesher has
    // built and nothing else has touched it. That is the control on the pruning below.
    const usize materials_from_voxels = mesher.palette().size();
    const ws::bake::PaintExport painted = ws::bake::build_paint_export(
        script.field, script.paint, paint_amplitude, script.origin_shift, shape_low, shape_high,
        [&mesher](ws::VoxelTypeId type) -> u32 { return mesher.material_of(type); });
    if (painted.unknown_ops > 0 || painted.order_faults > 0) {
        WS_LOG_WARN("bake_web", "{}: {} paint nodes have no op number and {} are out of order",
                    walk.clip, painted.unknown_ops, painted.order_faults);
    }

    // Did the pruning throw away a rule that actually fires?
    //
    // Not an argument, a control. The sampler has just painted this clip with the WHOLE stack, and
    // every material in the palette above is one it put on a voxel. If a rule that survived pruning
    // cannot account for one of them, a rule that did not survive was painting it, and the box that
    // said otherwise is wrong. It is the only check on this that is not downstream of the same
    // reasoning the pruning is made of -- and three audits agreeing is not evidence when all three
    // read one source.
    {
        std::vector<u8> reachable(materials_from_voxels, 0);
        for (const ws::bake::PaintRecord& rule : painted.rules) {
            if (rule.material < materials_from_voxels) reachable[rule.material] = 1;
        }
        usize orphans = 0;
        for (const u8 seen : reachable) { if (!seen) ++orphans; }
        if (orphans > 0) {
            WS_LOG_WARN("bake_web",
                        "{}: {} of {} materials on the voxels are painted by no surviving rule",
                        walk.clip, orphans, materials_from_voxels);
        }
    }
    // <<< paintexport

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
    put_u32(out, 4, kFormatVersion);
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
    // What this file was made from, so the next bake can tell at a glance whether it still holds:
    // the spliced source of the program that produced it, the settings it was sampled under, and
    // the code that did the sampling, all in one number. See `reuse` in main.
    put_u32(out, 180, static_cast<u32>(baked.key & 0xFFFFFFFFull));
    put_u32(out, 184, static_cast<u32>(baked.key >> 32));
    put_u32(out, 188, static_cast<u32>(walk.shapes.size()));
    // The cutter pool: how many, and where it starts. The offset is derivable from everything
    // above it, and it is written down anyway -- a reader that computes it agrees with a reader
    // that reads it, or one of the two is wrong and says so.
    put_u32(out, 192, static_cast<u32>(walk.cutter_count()));
    // 196 is filled in below, once the blocks in front of it have been appended.
    // >>> gi
    // 200..207 is the chunk directory, and it is filled in below for the same reason 196 is: it
    // points past every block in front of it. See `chunks`, at the end of this function.
    // <<< gi

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
    {
        const auto push_u32 = [&out](u32 bits) {
            out.push_back(static_cast<u8>(bits & 0xFFu));
            out.push_back(static_cast<u8>((bits >> 8) & 0xFFu));
            out.push_back(static_cast<u8>((bits >> 16) & 0xFFu));
            out.push_back(static_cast<u8>((bits >> 24) & 0xFFu));
        };
        const auto put = [&push_u32](f32 value) {
            u32 bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            push_u32(bits);
        };
        const usize shapes_at = out.size();
        for (const Shape& shape : walk.shapes) {
            push_u32(shape.op);
            push_u32(shape.cut_start);
            put(shape.scale);
            for (i32 i = 0; i < 12; ++i) put(shape.world_to_local[i]);
            for (i32 i = 0; i < 8; ++i) put(shape.a[i]);
            for (i32 i = 0; i < 3; ++i) put(shape.low[i]);
            for (i32 i = 0; i < 3; ++i) put(shape.high[i]);
            push_u32(shape.cut_count);
        }
        if (out.size() - shapes_at != walk.shapes.size() * kShapeBytes) {
            std::printf("  ! shape record is %zu bytes, not %zu\n",
                        walk.shapes.size() ? (out.size() - shapes_at) / walk.shapes.size() : 0,
                        kShapeBytes);
            return false;
        }
        put_u32(out, 196, static_cast<u32>(out.size()));
        for (const f32 value : walk.pool) put(value);
    }

    // >>> paintexport
    // The named blocks, after everything at a fixed offset. Anything added later is one more
    // entry in this vector and moves none of the file in front of it.
    {
        std::vector<Chunk> chunks;
        chunks.push_back(make_chunk("FLDG", ws::bake::field_chunk(painted)));
        chunks.push_back(make_chunk("PANT", ws::bake::paint_chunk(painted)));
    // <<< paintexport
    // >>> gi
        // The colour bounce, one RGB per lattice point. Absent on a clip that has no bounce to
        // carry, and a reader that asks for `GIRR` and gets null simply does without it.
        if (!gi.empty()) chunks.push_back(make_chunk("GIRR", gi.chunk()));
    // <<< gi
    // >>> ao
        // `AOCC`, the ambient-occlusion atlas: a small header, then one byte per exposed voxel
        // face. The viewer re-derives where each quad's run starts by prefix-summing `w * h`, so
        // the file carries no per-quad offset; the total is written down at 12 and checked at
        // load, so the two derivations either agree or one of them says so.
        {
            std::vector<u8> ao_bytes(32, 0);
            put_u32(ao_bytes, 0, 1);   // the chunk's own version, which is not the file's
            put_u32(ao_bytes, 4, occlusion.texel_count);
            put_u32(ao_bytes, 8, occlusion.atlas_width);
            put_u32(ao_bytes, 12, opaque_total + transparent_total);
            put_f32(ao_bytes, 16, occlusion.radius);
            put_u32(ao_bytes, 20, occlusion.rays);
            ao_bytes.insert(ao_bytes.end(), occlusion.texels.begin(), occlusion.texels.end());
            chunks.push_back(make_chunk("AOCC", std::move(ao_bytes)));
        }
    // <<< ao
    // >>> probes
        // The reflection probes: an octahedral atlas and the grid that says which probe is where.
        // Empty on a clip where no lattice point is in air and near matter, and a reader that gets
        // null for `RPRB` falls back to the sky exactly as it did.
        if (!probe_bytes.empty()) chunks.push_back(make_chunk("RPRB", probe_bytes));
    // <<< probes
    // >>> lights
        // `LGTS`, every emissive surface of the clip gathered into a list a fragment can walk.
        // Absent when nothing in the clip emits, and a reader that gets null lights the scene by
        // sun and sky exactly as it did.
        if (!lights.lights.empty()) chunks.push_back(make_chunk("LGTS", ws::web::write_lights(lights)));
    // <<< lights
    // >>> matvol
        // `MVOL` is what the matter INSIDE the clip is -- a paged material volume with a block
        // index -- and `THCK` is how thick it is, which is the second channel of MVOL's own pages
        // and is meaningless without it. The two go together or neither goes.
        {
            std::vector<u8> material_bytes = ws::bake::matvol::material_chunk(volume);
            if (!material_bytes.empty()) {
                chunks.push_back(make_chunk("MVOL", std::move(material_bytes)));
                chunks.push_back(make_chunk("THCK",
                                            ws::bake::matvol::thickness_chunk(volume)));
            }
        }
    // <<< matvol
    // >>> paintexport
        append_chunks(out, chunks);
    }
    // <<< paintexport

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

    // And throw away any compressed copy left over from a previous bake.
    //
    // The viewer asks for `<id>.wsc.gz` first and only falls back to the plain file, so a stale
    // `.gz` beside a fresh `.wsc` is not a slow path -- it is the OLD CLIP served in place of the
    // new one, with the index's own hash on the URL saying it is current. A whole rebuild of the
    // building was looked at and reported as unchanged because of exactly this.
    std::error_code gone;
    fs::remove(options.out / (baked.id + ".wsc.gz"), gone);

    std::printf("  %d/m  %d x %d x %d  %u quads  %zu shapes  %zu cutters  %zu materials  %.1f MB  "
                "%.1f s\n",
                metre,
                clip.size[0], clip.size[1], clip.size[2], baked.quads, walk.shapes.size(),
                walk.cutter_count(), mesher.palette().size(),
                static_cast<f64>(out.size()) / (1024.0 * 1024.0), seconds);
    // >>> ao
    std::printf("      ao: %u texels at 1/%d m  %.2f MB  %.1f s  radius %.2f m  %u rays\n",
                occlusion.texel_count, metre,
                static_cast<f64>(occlusion.texels.size()) / (1024.0 * 1024.0), ao_seconds,
                static_cast<f64>(occlusion.radius), occlusion.rays);
    // <<< ao
    // >>> probes
    if (probes.count > 0) {
        std::printf("      probes: %u at %.1f m, %dx%d x %d levels, %llu rays, %.2f MB, %.1f s\n",
                    probes.count, probes.spacing, probes.base, probes.base, probes.levels,
                    static_cast<unsigned long long>(probes.rays),
                    static_cast<f64>(probe_bytes.size()) / (1024.0 * 1024.0), probes.seconds);
    } else {
        std::printf("      probes: none -- no lattice point in this clip is in air and near matter\n");
    }
    // <<< probes
    // >>> lights
    // What the light list found, said out loud for the same reason as everything below it: a cap
    // that bites silently reads as "there were only that many". `unshadowed` is the number the
    // report has to carry, because an unshadowed lamp is the one that lights the room next door.
    if (!lights.lights.empty() || lights.clusters > 0) {
        f64 area = 0.0;
        f64 brightest = 0.0;
        for (const ws::web::Light& one : lights.lights) {
            area += static_cast<f64>(one.area);
            brightest = std::max(brightest, one.power());
        }
        const usize unshadowed = lights.lights.size() - lights.shadowed;
        std::printf("      lights: %zu from %zu clusters  %zu shadowed  %zu NOT  %.3f m2 emitting  "
                    "brightest %.2f  atlas %d x %d (%.0f kB)  %.1f s\n",
                    lights.lights.size(), lights.clusters, lights.shadowed, unshadowed, area,
                    brightest, lights.atlas_w, lights.atlas_h,
                    static_cast<f64>(lights.atlas.size()) / 1024.0, lights.seconds);
        if (lights.dropped > 0 || lights.dark > 0) {
            std::printf("      lights: %zu past the %zu cap were dropped, %zu clusters emit from no "
                        "face at all\n",
                        lights.dropped, ws::web::kMaxLights, lights.dark);
        }
    }
    // <<< lights
    // >>> matvol
    // The volume's real size against its dense size, per clip, every bake. This is the number the
    // decision to ship it rests on and it is not an estimate: a dense byte-pair per occupancy cell
    // is what the brief asked for and what the facility cannot afford, and the ratio here is what
    // says the block index earns its complexity.
    if (!volume.empty()) {
        std::printf("      matvol: %d x %d x %d cells  %zu materials  %zu solid  "
                    "%zu uniform + %zu paged blocks  %.2f MB packed vs %.2f MB dense (%.1f%%)  "
                    "%.1f s\n",
                    volume.dims[0], volume.dims[1], volume.dims[2], volume.palette.size(),
                    volume.solid_cells, volume.uniform_blocks, volume.page_blocks,
                    static_cast<f64>(volume.packed_bytes()) / (1024.0 * 1024.0),
                    static_cast<f64>(volume.dense_bytes()) / (1024.0 * 1024.0),
                    volume.dense_bytes() > 0 ? 100.0 * static_cast<f64>(volume.packed_bytes()) /
                                                   static_cast<f64>(volume.dense_bytes())
                                             : 0.0,
                    matvol_seconds);
        if (volume.dropped_materials > 0 || volume.clamped_cells > 0) {
            std::printf("      matvol: %zu materials dropped past %zu (%zu cells remapped)  "
                        "%zu cells thicker than 255 voxels, held there\n",
                        volume.dropped_materials, ws::bake::matvol::kMaxPalette,
                        volume.remapped_cells, volume.clamped_cells);
        }
    }
    // <<< matvol
    // What the shapes view is not showing exactly, said out loud. A silent truncation reads as
    // "it worked", which is the whole reason these are counted at all.
    if (walk.over_cap > 0 || walk.pool_full > 0 || walk.sub_intersections > 0 ||
        walk.sub_differences > 0 || walk.cutters_skipped > 0) {
        std::printf("      shapes view: %zu over the %zu-cutter cap (worst %zu)  "
                    "%zu dropped, pool full  %zu subtrahends with an intersection  "
                    "%zu with a difference  %zu unplaceable  pool %.2f MB\n",
                    walk.over_cap, ShapeWalk::kMaxCutters, walk.over_cap_worst, walk.pool_full,
                    walk.sub_intersections, walk.sub_differences, walk.cutters_skipped,
                    static_cast<f64>(walk.cutter_count() * kCutterBytes) / (1024.0 * 1024.0));
    }
    // >>> paintexport
    // What the shading has to walk. Printed for every clip because it is the number the budget
    // conversation starts from and there is no picture that shows it.
    std::printf("      paint: %zu of %zu rules (%zu pruned, %zu placed, %zu costly)  "
                "%zu nodes deep %zu  FLDG %.1f kB  PANT %.1f kB\n",
                painted.rules.size(), painted.rules_in, painted.rules_pruned, painted.rules_placed,
                painted.rules_costly, painted.nodes.size(), painted.deepest,
                static_cast<f64>(painted.field_bytes()) / 1024.0,
                static_cast<f64>(painted.paint_bytes()) / 1024.0);
    // <<< paintexport
    // >>> gi
    // What the indirect volume cost, every time, so a lattice that has quietly grown to megabytes
    // is a number somebody reads rather than a file that got bigger.
    std::printf("      indirect: %d x %d x %d at %.2f m  %zu of %zu points lit  %zu surfaces "
                "(%zu emitting)  %.0f KB  %.1f s\n",
                gi.dims[0], gi.dims[1], gi.dims[2], static_cast<f64>(gi.cell), gi_stats.lit_points,
                gi_stats.points, gi_stats.surface_cells, gi_stats.emissive_cells,
                static_cast<f64>(gi_stats.bytes) / 1024.0, gi_stats.seconds);
    // <<< gi
    if (options.verbose) {
        std::printf("      box  %.2f %.2f %.2f  ..  %.2f %.2f %.2f      matter  %.2f %.2f %.2f  "
                    ".. %.2f %.2f %.2f\n",
                    settings.low.x, settings.low.y, settings.low.z, settings.high.x,
                    settings.high.y, settings.high.z, matter_low[0], matter_low[1], matter_low[2],
                    matter_high[0], matter_high[1], matter_high[2]);
    }
    return true;
}

// >>> paintexport
// --------------------------------------------------------------------------------------
// Reading the paint back out of a baked file
//
// This is the acceptance test for the two chunks above and it is worth more here than a picture
// would be. There is nothing to look at: the output is data, and the only question that matters is
// whether the rules in the file are the rules the `.clip` actually wrote, in the order it wrote
// them, with the materials it named. So the file is read back the way the viewer will read it --
// through the chunk directory, off the bytes on disk, with nothing kept from the bake -- and
// printed beside the numbers a person can check against the source.
//
// It prints the material's colour rather than its name, because a name is not in the file and a
// colour is: `material moss rgb=64,112,54` in the source and `rgb 64,112,54` here is a check that
// crosses the format rather than one taken from inside it.
// --------------------------------------------------------------------------------------

void dump_node(const std::vector<u8>& bytes, usize base, u32 count, u32 index, i32 depth,
               i32 max_depth) {
    if (index >= count) {
        std::printf("%*s<node %u, and there are %u>\n", depth * 2 + 6, "", index, count);
        return;
    }
    const usize at = base + 4 + static_cast<usize>(index) * ws::bake::kFieldNodeBytes;
    const u32 op = static_cast<u32>(read_u32(bytes, at));
    const u32 children = static_cast<u32>(read_u32(bytes, at + 4));

    char args[256];
    usize written = 0;
    i32 last = -1;
    for (i32 i = 0; i < 8; ++i) {
        if (read_f32(bytes, at + 24 + static_cast<usize>(i) * 4) != 0.0f) last = i;
    }
    for (i32 i = 0; i <= last && written + 16 < sizeof(args); ++i) {
        const f32 value = read_f32(bytes, at + 24 + static_cast<usize>(i) * 4);
        const int put = std::snprintf(args + written, sizeof(args) - written, " %g",
                                      static_cast<f64>(value));
        if (put > 0) written += static_cast<usize>(put);
    }
    args[written] = '\0';

    const f32 lo0 = read_f32(bytes, at + 56);
    const bool boxed = std::abs(static_cast<f64>(lo0)) < 1e29;
    std::printf("%*s[%u] %s%s%s\n", depth * 2 + 6, "", index, ws::bake::field_op_name(op), args,
                boxed ? "" : "   (no box)");
    if (children == 0) return;
    if (depth >= max_depth) {
        std::printf("%*s... %u more\n", (depth + 1) * 2 + 6, "", children);
        return;
    }
    for (u32 c = 0; c < children && c < 4; ++c) {
        dump_node(bytes, base, count, static_cast<u32>(read_u32(bytes, at + 8 + c * 4)), depth + 1,
                  max_depth);
    }
}

int dump_paint(const Options& options) {
    const fs::path file = options.out / (options.dump_paint + ".wsc");
    std::ifstream stream(file, std::ios::binary | std::ios::ate);
    if (!stream) {
        std::printf("no baked clip at %s\n", file.string().c_str());
        return 1;
    }
    const std::streamsize size = stream.tellg();
    stream.seekg(0);
    std::vector<u8> bytes(static_cast<usize>(size));
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), size)) {
        std::printf("cannot read %s\n", file.string().c_str());
        return 1;
    }
    if (bytes.size() < kHeaderBytes || bytes[0] != 'W' || bytes[1] != 'S' || bytes[2] != 'C' ||
        bytes[3] != 'V') {
        std::printf("%s is not a clip file\n", file.string().c_str());
        return 1;
    }
    const u32 version = static_cast<u32>(read_u32(bytes, 4));
    if (version != kFormatVersion) {
        std::printf("%s is version %u and this reads %u\n", file.string().c_str(), version,
                    kFormatVersion);
        return 1;
    }

    const u32 materials = static_cast<u32>(read_u32(bytes, 48));
    const u32 directory = static_cast<u32>(read_u32(bytes, 200));
    const u32 chunks = static_cast<u32>(read_u32(bytes, 204));
    std::printf("%s  %.2f MB  %u materials  %u chunks at %u\n", file.string().c_str(),
                static_cast<f64>(bytes.size()) / (1024.0 * 1024.0), materials, chunks, directory);

    usize field_at = 0, field_size = 0, paint_at = 0, paint_size = 0;
    for (u32 i = 0; i < chunks; ++i) {
        const usize entry = static_cast<usize>(directory) + static_cast<usize>(i) * 16;
        if (entry + 16 > bytes.size()) break;
        const char name[5] = {static_cast<char>(bytes[entry]), static_cast<char>(bytes[entry + 1]),
                              static_cast<char>(bytes[entry + 2]),
                              static_cast<char>(bytes[entry + 3]), '\0'};
        const usize at = static_cast<usize>(read_u32(bytes, entry + 4));
        const usize length = static_cast<usize>(read_u32(bytes, entry + 8));
        std::printf("  %-4s  offset %8zu  %8zu bytes\n", name, at, length);
        if (std::strcmp(name, "FLDG") == 0) {
            field_at = at;
            field_size = length;
        } else if (std::strcmp(name, "PANT") == 0) {
            paint_at = at;
            paint_size = length;
        }
    }
    if (field_size == 0 || paint_size == 0) {
        std::printf("  no FLDG or no PANT in this file\n");
        return 1;
    }

    const u32 node_count = static_cast<u32>(read_u32(bytes, field_at));
    const u32 rule_count = static_cast<u32>(read_u32(bytes, paint_at));
    if (4 + static_cast<usize>(node_count) * ws::bake::kFieldNodeBytes != field_size) {
        std::printf("  ! FLDG says %u nodes, which is not %zu bytes\n", node_count, field_size);
        return 1;
    }
    if (4 + static_cast<usize>(rule_count) * ws::bake::kPaintRuleBytes != paint_size) {
        std::printf("  ! PANT says %u rules, which is not %zu bytes\n", rule_count, paint_size);
        return 1;
    }
    std::printf("  %u nodes, %u rules\n\n", node_count, rule_count);

    const i32 max_depth = options.verbose ? 64 : 4;
    for (u32 i = 0; i < rule_count; ++i) {
        const usize at = paint_at + 4 + static_cast<usize>(i) * ws::bake::kPaintRuleBytes;
        const u32 root = static_cast<u32>(read_u32(bytes, at));
        const f32 below = read_f32(bytes, at + 4);
        const f32 above = read_f32(bytes, at + 8);
        const i32 facing = static_cast<i32>(read_u32(bytes, at + 12));
        const f32 facing_at = read_f32(bytes, at + 16);
        const u32 material = static_cast<u32>(read_u32(bytes, at + 20));
        const u32 flags = static_cast<u32>(read_u32(bytes, at + 24));

        char colour[32] = "?";
        if (material < materials) {
            const usize record = kHeaderBytes + static_cast<usize>(material) * 16;
            std::snprintf(colour, sizeof(colour), "%u,%u,%u", bytes[record], bytes[record + 1],
                          bytes[record + 2]);
        }
        char band[64];
        std::snprintf(band, sizeof(band), "[%s, %s]",
                      (above <= -1e29f) ? "-inf" : std::to_string(above).c_str(),
                      (below >= 1e29f) ? "+inf" : std::to_string(below).c_str());
        char face[48] = "";
        if (facing >= 0) {
            std::snprintf(face, sizeof(face), "  facing %c %s %g", "xyz"[facing & 3],
                          (facing_at >= 0.0f) ? ">=" : "<=", static_cast<f64>(facing_at));
        }
        std::printf("rule %-3u  material %-3u rgb %-12s band %-24s%s\n", i, material, colour, band,
                    face);
        std::printf("         flags%s%s%s%s%s%s%s\n",
                    (flags & ws::bake::kRuleMetric) ? " metric" : "",
                    (flags & ws::bake::kRuleBounded) ? " bounded" : "",
                    (flags & ws::bake::kRuleFacing) ? " facing" : "",
                    (flags & ws::bake::kRuleBoxed) ? " boxed" : "",
                    (flags & ws::bake::kRulePlaced) ? " placed" : "",
                    (flags & ws::bake::kRuleCostly) ? " costly" : "",
                    (flags & ws::bake::kRuleUndercoat) ? " undercoat" : "");
        if (flags & ws::bake::kRuleBoxed) {
            std::printf("         box  %.2f %.2f %.2f  ..  %.2f %.2f %.2f\n",
                        static_cast<f64>(read_f32(bytes, at + 28)),
                        static_cast<f64>(read_f32(bytes, at + 32)),
                        static_cast<f64>(read_f32(bytes, at + 36)),
                        static_cast<f64>(read_f32(bytes, at + 40)),
                        static_cast<f64>(read_f32(bytes, at + 44)),
                        static_cast<f64>(read_f32(bytes, at + 48)));
        }
        dump_node(bytes, field_at, node_count, root, 0, max_depth);
        std::printf("\n");
    }
    return 0;
}
// <<< paintexport

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
        } else if (arg == "--part-metre") {
            options.part_metre = std::stoi(next("--part-metre"));
        } else if (arg == "--only") {
            options.only = next("--only");
        } else if (arg == "--branch") {
            options.branch = next("--branch");
        } else if (arg == "--commit") {
            options.commit = next("--commit");
        } else if (arg == "--shard") {
            const std::string value = next("--shard");
            const usize slash = value.find('/');
            if (slash == std::string::npos) {
                std::printf("--shard wants INDEX/COUNT, like 3/12\n");
                return 2;
            }
            options.shard = std::stoi(value.substr(0, slash));
            options.shards = std::max(1, std::stoi(value.substr(slash + 1)));
        } else if (arg == "--code-hash") {
            options.code_hash = next("--code-hash");
        // >>> paintexport
        } else if (arg == "--dump-paint") {
            options.dump_paint = next("--dump-paint");
        // <<< paintexport
        } else if (arg == "--index-only") {
            options.index_only = true;
        } else if (arg == "--force") {
            options.force = true;
        } else if (arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            std::printf(
                "ws_bake_web - bake every clip for the viewer at web/\n\n"
                "  --clips DIR      where the clips are (default clips)\n"
                "  --out DIR        where the .wsc files go (default web/data)\n"
                "  --budget N       cells a sampled box may hold before the resolution halves\n"
                "  --max-metre N    never sample finer than this\n"
                "  --part-metre N   and never finer than this for one part of a manifest\n"
                "  --only ID        bake one clip, by its id (facility, facility-dome, ...)\n"
                "  --branch NAME    what the index should say these clips came from\n"
                "  --commit SHA     and at which commit\n"
                "  --code-hash H    a hash of the sampler's own sources; changing it rebakes all\n"
                "  --force          bake every clip even if its key says it is unchanged\n"
                "  --shard I/N      bake only every Nth clip, starting at I, for a parallel bake\n"
                "  --index-only     index what is already baked and sample nothing\n"
                // >>> paintexport
                "  --dump-paint ID  print the paint stack and field graph of a baked clip\n"
                // <<< paintexport
                );
            return 0;
        } else {
            std::printf("unknown argument %s\n", arg.c_str());
            return 2;
        }
    }

    // >>> paintexport
    // Before anything is read or written: the two op tables must agree, or the file's shapes and
    // its paint fields disagree about what a cylinder is.
    if (!op_numbering_agrees()) {
        std::printf("the shapes view and the paint field number the solids differently\n");
        return 2;
    }
    if (!options.dump_paint.empty()) return dump_paint(options);
    // <<< paintexport

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

    // After the sort, so every runner splits the same list the same way whatever order the
    // directory happened to be walked in.
    if (options.shards > 1) {
        std::vector<fs::path> mine;
        for (usize i = 0; i < files.size(); ++i) {
            if (static_cast<i32>(i % static_cast<usize>(options.shards)) == options.shard) {
                mine.push_back(files[i]);
            }
        }
        std::printf("shard %d of %d: %zu of %zu clips\n", options.shard, options.shards,
                    mine.size(), files.size());
        files.swap(mine);
    }

    // Every core, not every core minus two. That default leaves room for a main thread and a
    // simulation thread, which is right in the game and is two idle cores in a job that does
    // nothing else. On the four-core runner it is the difference between two workers and four.
    ws::JobSystem jobs(std::max(1u, std::thread::hardware_concurrency()));
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
    // What a clip is MADE of, hashed: its own text and every file it includes, spliced the way the
    // parser splices them. An edit to `_contract.clip` moves this for every fragment that includes
    // it, which is exactly the set that has to be rebaked.
    const auto splice_hash = [&](const fs::path& path) -> u64 {
        std::vector<ws::forge::SourceLine> origin;
        std::vector<ws::forge::ScriptError> errors;
        const std::string text =
            ws::forge::expand_includes(path.string(), origin, errors, options.clips.string());
        if (!errors.empty()) return 0;   // 0 never matches a stored key, so it rebakes
        return fnv1a(reinterpret_cast<const u8*>(text.data()), text.size());
    };
    const u64 code_seed = options.code_hash.empty()
                         ? 0x9e3779b97f4a7c15ull
                         : fnv1a(reinterpret_cast<const u8*>(options.code_hash.data()),
                                 options.code_hash.size());

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

    const u64 manifest_source = manifest.parsed ? splice_hash(manifest_path) : 0;

    for (const fs::path& file : files) {
        const fs::path relative = fs::relative(file, options.clips);
        const std::string id = identifier(relative);
        if (!options.only.empty() && options.only != id) continue;
        std::printf("%s\n", relative.generic_string().c_str());

        const std::string stem = file.stem().string();
        Baked baked;
        bool built = false;

        u32 root = 0;
        const bool from_manifest =
            manifest.parsed && (relative == fs::path("facility.clip") ||
                                manifest.script.part("part_" + stem, root));

        // A fragment's file is not what decides its voxels -- the whole manifest is, because the
        // part is intersected with the building's own solid and painted with the building's own
        // stack. So a fragment is keyed on the MANIFEST'S splice, and an edit to any fragment
        // rebakes the building and all of its parts. That is not conservatism, it is the
        // dependency: they really do all change.
        const u64 source = from_manifest ? manifest_source : splice_hash(file);
        u64 key = 0;
        if (source != 0) {
            char settings[160];
            std::snprintf(settings, sizeof(settings), "%s|%d|%d|%lld|%d", id.c_str(),
                          options.max_metre, options.part_metre,
                          static_cast<long long>(options.budget), from_manifest ? 1 : 0);
            key = fnv1a(reinterpret_cast<const u8*>(settings), std::strlen(settings), code_seed);
            key = fnv1a(reinterpret_cast<const u8*>(&source), sizeof(source), key);
        }
        if (key != 0 && reuse(options, relative, key, baked)) {
            done.push_back(baked);
            continue;
        }
        if (options.index_only) {
            std::printf("  not baked yet\n");
            continue;
        }
        baked.key = key;

        root = 0;
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

    // Anything in the output that is no longer a clip. The cache carries files between runs, so a
    // clip that was deleted or renamed would otherwise sit in the published site forever, absent
    // from the index and downloadable by anybody who still had its URL.
    //
    // Never while sharding: a shard's output directory holds one twelfth of the clips by design,
    // and a sweep there would delete the other eleven twelfths as soon as they were merged.
    if (options.shards == 1 && !options.index_only) {
        std::vector<std::string> keep;
        for (const Baked& baked : done) keep.push_back(baked.id);
        std::error_code walk;
        for (const fs::directory_entry& entry : fs::directory_iterator(options.out, walk)) {
            const std::string name = entry.path().filename().string();
            const std::string suffix = (name.size() > 7 && name.compare(name.size() - 7, 7, ".wsc.gz") == 0)
                                           ? ".wsc.gz"
                                           : ((name.size() > 4 && name.compare(name.size() - 4, 4, ".wsc") == 0)
                                                  ? ".wsc"
                                                  : "");
            if (suffix.empty()) continue;
            const std::string id = name.substr(0, name.size() - suffix.size());
            if (std::find(keep.begin(), keep.end(), id) != keep.end()) continue;
            std::error_code gone;
            fs::remove(entry.path(), gone);
            std::printf("dropped %s, which is no longer a clip\n", name.c_str());
        }
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
    json += "  \"branch\": \"" + json_escape(options.branch) + "\",\n";
    json += "  \"commit\": \"" + json_escape(options.commit) + "\",\n";
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
    return (done.empty() && !options.index_only) ? 1 : 0;
}
