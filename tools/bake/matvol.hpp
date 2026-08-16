// The material volume and the thickness field: what the stone INSIDE a wall is made of, and how
// far a ray would travel through it.
//
// documentation/24-clip-viewer.md §6 is what this is for. In one paragraph: a `.wsc` carries the
// exposed surface as greedy-meshed quads and a one-bit occupancy grid, and NOTHING in it can say
// what the matter at a given point inside a wall is. So the slice cap paints the whole cut in one
// colour -- the clip's commonest opaque material, picked by area at load -- and a cut through the
// rotunda's porphyry-and-lapis floor comes out the colour of the building's limestone. Refraction
// and translucency have the same hole from the other end: `alabaster` is translucent 210 and the
// three coloured glasses carry a Beer-Lambert `absorb` per metre, and a per-metre absorption is
// meaningless without a distance.
//
// Both are one missing thing, and this is it.
//
// # Two channels on one partition, and the size is the whole design
//
//   material   one byte a cell, indexing the volume's own palette. 0 is air.
//   thickness  one byte a cell, in the CLIP'S OWN VOXELS: the thinnest run of matter through that
//              cell, over the three axes. A pane comes out its pane thickness and a wall comes out
//              its wall thickness, which is what a refraction ray at normal incidence travels and
//              what Beer-Lambert wants a length for.
//
// The cells are the occupancy grid's -- 12.5 cm for anything sampled at 8 voxels to the metre or
// finer -- because that grid already exists, the viewer already carries it, and the alternative
// (the full voxel lattice) is eight times the cells for a cut face nobody reads at that pitch.
//
// **Dense, that is unaffordable and this is the measurement rather than the fear.** The facility
// at 16 voxels to the metre is 9.0 million occupancy cells, so two dense byte planes are 18 MB
// against the 6.5 MB its quads cost. Nobody is downloading that onto a phone.
//
// So the volume is **sparse by 4x4x4 block**, and the reason it works is that a building is
// mostly air and mostly uniform: a block wholly inside a wall is one stone at one thickness, a
// block of open sky is air at zero, and only blocks the surface actually passes through hold
// anything. A block that is one value in BOTH channels is stored as one word in the directory; the
// rest are pages of 64 cells, two bytes each. The directory is four bytes a block, which is 3.1%
// of the dense size, and it is the same partition for both channels so the two can never disagree
// about where a page is.
//
// **Four and not eight, and that is measured.** A block only costs a page when a SURFACE runs
// through it, and a building's surfaces are everywhere, so what matters is how much air and solid
// each straddling block drags in with it. On facility/rotunda the same volume is 0.65 MB at four
// and 0.95 MB at eight; on glass_test 0.07 against 0.22; on sampler 0.09 against 0.15. Sixteen is
// worse again in every case. Two would halve the pages once more and the directory alone would
// then be a quarter of the dense size, which is the other end of the same curve.
//
// That structure is not decompressed to draw. It goes to the card AS the two textures the shader
// reads -- a block directory and a page atlas -- so what it costs in VRAM is what it costs on the
// wire, which is the whole reason it is a block index and not a run length.
//
// # More than 255 materials
//
// A byte indexes 255 of them and air. Clips run under that today (the whole facility is 203 at 16
// to the metre) but one will not, and a silent truncation is the failure this repository keeps
// writing down. So the volume carries **its own palette**: the 255 materials with the most CELLS,
// and everything past that mapped to the nearest kept one by colour, opacity and translucency. The
// count of remapped cells is returned and the baker warns with it. A clip over the line therefore
// loses the rarest matter inside its walls to a stone that looks like it, and says so; it does not
// lose the volume, and it does not lose the common case.
//
// # The file
//
// Two chunks in the version 3 chunk directory. THCK is the SECOND CHANNEL of MVOL's own pages and
// is not readable without it -- that is deliberate, because one partition described twice is a
// partition that can drift.
//
//   MVOL   0 u32 dims[3]          cells
//         12 u32 cells_per_metre
//         16 u32 voxels_per_metre the clip's own; the unit the thickness bytes count in
//         20 u32 block_dim        4, and the shader's shifts assume it
//         24 u32 blocks[3]
//         36 u32 palette_count    <= 255
//         40 u32 block_count
//         44 u32 page_count
//         48 u16 palette[palette_count]   volume value v -> clip material palette[v - 1]
//            (padded to a multiple of four)
//            u32 index[block_count]
//            u8  material_pages[page_count * 64]
//
//   THCK   0 u32 page_count       must equal MVOL's
//          4 u32 voxels_per_metre the unit again, so a reader that only wants thickness can check
//          8 u8  thickness_pages[page_count * 64]
//
//   index entry:  bit 31 set -> the block is uniform, bits 0..7 its material and 8..15 its
//                 thickness.  bit 31 clear -> the ordinal of its page.
#pragma once

#include <algorithm>
#include <cstring>
#include <functional>
#include <vector>

#include "core/types.hpp"
#include "world/voxel_type.hpp"

namespace ws::bake::matvol {

using ws::f64;
using ws::i32;
using ws::u16;
using ws::u32;
using ws::u8;
using ws::usize;

constexpr i32 kBlockDim = 4;
constexpr usize kPageCells = static_cast<usize>(kBlockDim) * kBlockDim * kBlockDim;
constexpr usize kMaxPalette = 255;

struct Volume {
    i32 dims[3]{0, 0, 0};        // cells
    i32 blocks[3]{0, 0, 0};
    i32 cells_per_metre = 1;
    i32 voxels_per_metre = 1;
    std::vector<u16> palette;    // volume value v (1..) -> the clip's material index
    std::vector<u32> index;      // one per block
    std::vector<u8> material_pages;
    std::vector<u8> thickness_pages;

    // What it cost and what it had to give up, for the line the baker prints.
    usize cells = 0;
    usize uniform_blocks = 0;
    usize page_blocks = 0;
    usize solid_cells = 0;
    usize remapped_cells = 0;    // matter past the 255th material, painted as its nearest kept one
    usize clamped_cells = 0;     // matter thicker than 255 voxels, held at 255
    usize dropped_materials = 0;

    bool empty() const { return index.empty(); }
    usize dense_bytes() const { return cells * 2; }
    usize packed_bytes() const {
        return index.size() * 4 + material_pages.size() + thickness_pages.size() +
               palette.size() * 2;
    }
};

// How far apart two materials look, for the one decision this file makes on a clip's behalf. It is
// a colour distance with opacity and translucency in it, because a rare glass remapped to a stone
// of the same colour would be a hole in a wall rather than a slightly wrong stone.
inline f64 material_distance(const ws::VisualRecord& a, const ws::VisualRecord& b) {
    const f64 dr = static_cast<f64>(a.red) - static_cast<f64>(b.red);
    const f64 dg = static_cast<f64>(a.green) - static_cast<f64>(b.green);
    const f64 db = static_cast<f64>(a.blue) - static_cast<f64>(b.blue);
    const f64 da = static_cast<f64>(a.opacity) - static_cast<f64>(b.opacity);
    const f64 dt = static_cast<f64>(a.translucency) - static_cast<f64>(b.translucency);
    return dr * dr * 2.0 + dg * dg * 4.0 + db * db + da * da * 8.0 + dt * dt * 2.0;
}

// `solid_at` is the mesher's own occupancy test and is asked about every voxel; `material_at`
// gives the clip's own material index and is asked only about the solid ones, because it is the
// mesher's INTERNING call and it is not free. That call is also why the palette GROWS here: a
// stone that exists only inside a wall has never been seen by the surface mesher and has no index
// until this asks for one, which is why the volume is built before the header's material count is
// written.
//
// `size` is the voxel grid, `cells_per_metre` the occupancy grid's pitch, `voxels_per_metre` the
// clip's own. The two grids must divide: `voxels_per_metre % cells_per_metre == 0`.
inline Volume build(const i32 size[3], i32 voxels_per_metre, i32 cells_per_metre,
                    const std::vector<ws::VisualRecord>& palette,
                    const std::function<bool(i32, i32, i32)>& solid_at,
                    const std::function<i32(i32, i32, i32)>& material_at) {
    Volume out;
    out.voxels_per_metre = voxels_per_metre;
    out.cells_per_metre = cells_per_metre;
    const i32 step = std::max(1, voxels_per_metre / std::max(1, cells_per_metre));
    for (i32 axis = 0; axis < 3; ++axis) {
        out.dims[axis] = (size[axis] + step - 1) / step;
        out.blocks[axis] = (out.dims[axis] + kBlockDim - 1) / kBlockDim;
    }
    const usize cells = static_cast<usize>(out.dims[0]) * static_cast<usize>(out.dims[1]) *
                        static_cast<usize>(out.dims[2]);
    if (cells == 0) return out;
    out.cells = cells;

    const usize voxels = static_cast<usize>(size[0]) * static_cast<usize>(size[1]) *
                         static_cast<usize>(size[2]);
    const auto voxel_index = [size](i32 x, i32 y, i32 z) {
        return static_cast<usize>(x) + static_cast<usize>(y) * static_cast<usize>(size[0]) +
               static_cast<usize>(z) * static_cast<usize>(size[0]) * static_cast<usize>(size[1]);
    };

    // ---- the thinnest run of matter through every voxel -------------------------------------
    //
    // Three passes, one per axis, each taking the length of the maximal solid run a voxel is part
    // of and keeping the smallest seen so far. ONE byte a voxel and nothing else, because the
    // facility at 16 to the metre is 73 million voxels and a second plane of them is 73 MB for
    // nothing: air is 0 and a solid voxel's smallest run is at least 1, so the array is its own
    // occupancy test all the way through.
    std::vector<u8> run(voxels, 0);
    {
        for (i32 z = 0; z < size[2]; ++z) {
            for (i32 y = 0; y < size[1]; ++y) {
                for (i32 x = 0; x < size[0]; ++x) {
                    if (solid_at(x, y, z)) run[voxel_index(x, y, z)] = 255;
                }
            }
        }
        for (i32 axis = 0; axis < 3; ++axis) {
            const i32 other0 = (axis + 1) % 3;
            const i32 other1 = (axis + 2) % 3;
            for (i32 b = 0; b < size[other1]; ++b) {
                for (i32 a = 0; a < size[other0]; ++a) {
                    i32 at[3]{0, 0, 0};
                    at[other0] = a;
                    at[other1] = b;
                    i32 t = 0;
                    while (t < size[axis]) {
                        at[axis] = t;
                        if (run[voxel_index(at[0], at[1], at[2])] == 0) {
                            ++t;
                            continue;
                        }
                        i32 end = t;
                        while (end < size[axis]) {
                            at[axis] = end;
                            if (run[voxel_index(at[0], at[1], at[2])] == 0) break;
                            ++end;
                        }
                        const u8 held = static_cast<u8>(std::min(end - t, 255));
                        for (i32 s = t; s < end; ++s) {
                            at[axis] = s;
                            u8& cell = run[voxel_index(at[0], at[1], at[2])];
                            if (held < cell) cell = held;
                        }
                        t = end;
                    }
                }
            }
        }
    }

    // ---- what each cell is made of, and how thick it is --------------------------------------
    //
    // The occupancy grid is CONSERVATIVE -- a cell is solid if any voxel in it is -- so the cell's
    // material is the commonest non-air voxel in it and its thickness is the THICKEST run in it.
    // A cell straddling a thin fin and the wall behind it reads as the wall, which is the one a
    // refraction ray entering there would actually cross.
    std::vector<u16> cell_material(cells, 0);   // clip material index + 1; 0 is air
    std::vector<u8> cell_thickness(cells, 0);
    {
        i32 tally_material[64];
        i32 tally_count[64];
        for (i32 cz = 0; cz < out.dims[2]; ++cz) {
            for (i32 cy = 0; cy < out.dims[1]; ++cy) {
                for (i32 cx = 0; cx < out.dims[0]; ++cx) {
                    i32 kinds = 0;
                    u8 thickest = 0;
                    const i32 x0 = cx * step, y0 = cy * step, z0 = cz * step;
                    for (i32 z = z0; z < std::min(z0 + step, size[2]); ++z) {
                        for (i32 y = y0; y < std::min(y0 + step, size[1]); ++y) {
                            for (i32 x = x0; x < std::min(x0 + step, size[0]); ++x) {
                                const u8 thick = run[voxel_index(x, y, z)];
                                if (thick == 0) continue;
                                const i32 material = material_at(x, y, z);
                                if (material < 0) continue;
                                if (thick > thickest) thickest = thick;
                                i32 found = -1;
                                for (i32 k = 0; k < kinds; ++k) {
                                    if (tally_material[k] == material) {
                                        found = k;
                                        break;
                                    }
                                }
                                if (found >= 0) {
                                    ++tally_count[found];
                                } else if (kinds < 64) {
                                    tally_material[kinds] = material;
                                    tally_count[kinds] = 1;
                                    ++kinds;
                                }
                            }
                        }
                    }
                    if (kinds == 0) continue;
                    i32 best = 0;
                    for (i32 k = 1; k < kinds; ++k) {
                        if (tally_count[k] > tally_count[best]) best = k;
                    }
                    const usize at = static_cast<usize>(cx) +
                                     static_cast<usize>(cy) * static_cast<usize>(out.dims[0]) +
                                     static_cast<usize>(cz) * static_cast<usize>(out.dims[0]) *
                                         static_cast<usize>(out.dims[1]);
                    cell_material[at] = static_cast<u16>(tally_material[best] + 1);
                    cell_thickness[at] = thickest;
                    ++out.solid_cells;
                }
            }
        }
    }
    run.clear();
    run.shrink_to_fit();

    // ---- the volume's own palette ------------------------------------------------------------
    //
    // A byte holds 255 materials and air. Under that the palette is just the materials that
    // actually occur, in order of how much of the volume they are; over it, the 255 commonest are
    // kept and the rest are painted as whichever kept material looks most like them. Both counts
    // come back so the baker can say which happened.
    std::vector<usize> occurrences(palette.size(), 0);
    for (const u16 value : cell_material) {
        if (value != 0) ++occurrences[static_cast<usize>(value - 1)];
    }
    std::vector<u16> present;
    for (usize i = 0; i < occurrences.size(); ++i) {
        if (occurrences[i] > 0) present.push_back(static_cast<u16>(i));
    }
    std::sort(present.begin(), present.end(), [&occurrences](u16 a, u16 b) {
        if (occurrences[a] != occurrences[b]) return occurrences[a] > occurrences[b];
        return a < b;
    });
    if (present.size() > kMaxPalette) {
        out.dropped_materials = present.size() - kMaxPalette;
        present.resize(kMaxPalette);
    }
    out.palette = present;

    // clip material index -> volume value, and for a material that did not make the cut, the
    // value of the kept material nearest to it.
    std::vector<u8> to_value(palette.size(), 0);
    for (usize v = 0; v < present.size(); ++v) to_value[present[v]] = static_cast<u8>(v + 1);
    if (out.dropped_materials > 0) {
        for (usize i = 0; i < palette.size(); ++i) {
            if (to_value[i] != 0 || occurrences[i] == 0) continue;
            f64 nearest = 1e300;
            u8 pick = 1;
            for (usize v = 0; v < present.size(); ++v) {
                const f64 d = material_distance(palette[i], palette[present[v]]);
                if (d < nearest) {
                    nearest = d;
                    pick = static_cast<u8>(v + 1);
                }
            }
            to_value[i] = pick;
            out.remapped_cells += occurrences[i];
        }
    }

    // ---- PROBE ---------------------------------------------------------------------------
    {
        for (i32 dim : {4, 8, 16}) {
            i32 nb[3];
            for (i32 a = 0; a < 3; ++a) nb[a] = (out.dims[a] + dim - 1) / dim;
            usize um = 0, ut = 0, ub = 0, total = 0;
            for (i32 bz = 0; bz < nb[2]; ++bz)
             for (i32 by = 0; by < nb[1]; ++by)
              for (i32 bx = 0; bx < nb[0]; ++bx) {
                ++total;
                bool um_ = true, ut_ = true;
                u16 fm = 0; u8 ft = 0; bool first = true;
                for (i32 lz = 0; lz < dim; ++lz)
                 for (i32 ly = 0; ly < dim; ++ly)
                  for (i32 lx = 0; lx < dim; ++lx) {
                    const i32 cx = bx*dim+lx, cy = by*dim+ly, cz = bz*dim+lz;
                    u16 m = 0; u8 t = 0;
                    if (cx < out.dims[0] && cy < out.dims[1] && cz < out.dims[2]) {
                        const usize at = static_cast<usize>(cx) + static_cast<usize>(cy)*static_cast<usize>(out.dims[0]) + static_cast<usize>(cz)*static_cast<usize>(out.dims[0])*static_cast<usize>(out.dims[1]);
                        m = cell_material[at]; t = cell_thickness[at];
                    }
                    if (first) { fm = m; ft = t; first = false; }
                    else { if (m != fm) um_ = false; if (t != ft) ut_ = false; }
                  }
                if (um_) ++um;
                if (ut_) ++ut;
                if (um_ && ut_) ++ub;
              }
            const usize cellsper = static_cast<usize>(dim)*dim*dim;
            std::printf("      PROBE dim %2d: blocks %zu  mat-uniform %zu (%.2f MB)  "
                        "thick-uniform %zu (%.2f MB)  both %zu (%.2f MB joint)\n",
                dim, total, um,
                (total*4 + (total-um)*cellsper)/1048576.0,
                ut, (total*4 + (total-ut)*cellsper)/1048576.0,
                ub, (total*4 + (total-ub)*cellsper*2)/1048576.0);
        }
    }

    // ---- the blocks --------------------------------------------------------------------------
    const usize block_count = static_cast<usize>(out.blocks[0]) * static_cast<usize>(out.blocks[1]) *
                              static_cast<usize>(out.blocks[2]);
    out.index.assign(block_count, 0x80000000u);
    std::vector<u8> material_page(kPageCells, 0);
    std::vector<u8> thickness_page(kPageCells, 0);
    for (i32 bz = 0; bz < out.blocks[2]; ++bz) {
        for (i32 by = 0; by < out.blocks[1]; ++by) {
            for (i32 bx = 0; bx < out.blocks[0]; ++bx) {
                bool uniform = true;
                u8 first_material = 0;
                u8 first_thickness = 0;
                for (i32 lz = 0; lz < kBlockDim; ++lz) {
                    for (i32 ly = 0; ly < kBlockDim; ++ly) {
                        for (i32 lx = 0; lx < kBlockDim; ++lx) {
                            const i32 cx = bx * kBlockDim + lx;
                            const i32 cy = by * kBlockDim + ly;
                            const i32 cz = bz * kBlockDim + lz;
                            u8 material = 0;
                            u8 thickness = 0;
                            // Past the edge of the volume is air, so a block on the boundary is
                            // uniform whenever the cells inside it are.
                            if (cx < out.dims[0] && cy < out.dims[1] && cz < out.dims[2]) {
                                const usize at =
                                    static_cast<usize>(cx) +
                                    static_cast<usize>(cy) * static_cast<usize>(out.dims[0]) +
                                    static_cast<usize>(cz) * static_cast<usize>(out.dims[0]) *
                                        static_cast<usize>(out.dims[1]);
                                const u16 value = cell_material[at];
                                if (value != 0) {
                                    material = to_value[static_cast<usize>(value - 1)];
                                    thickness = cell_thickness[at];
                                    if (thickness == 255) ++out.clamped_cells;
                                }
                            }
                            const usize local = static_cast<usize>(lx) +
                                                static_cast<usize>(ly) * kBlockDim +
                                                static_cast<usize>(lz) * kBlockDim * kBlockDim;
                            material_page[local] = material;
                            thickness_page[local] = thickness;
                            if (local == 0) {
                                first_material = material;
                                first_thickness = thickness;
                            } else if (material != first_material || thickness != first_thickness) {
                                uniform = false;
                            }
                        }
                    }
                }
                const usize block = static_cast<usize>(bx) +
                                    static_cast<usize>(by) * static_cast<usize>(out.blocks[0]) +
                                    static_cast<usize>(bz) * static_cast<usize>(out.blocks[0]) *
                                        static_cast<usize>(out.blocks[1]);
                if (uniform) {
                    out.index[block] = 0x80000000u | static_cast<u32>(first_material) |
                                       (static_cast<u32>(first_thickness) << 8);
                    ++out.uniform_blocks;
                } else {
                    out.index[block] = static_cast<u32>(out.page_blocks);
                    out.material_pages.insert(out.material_pages.end(), material_page.begin(),
                                              material_page.end());
                    out.thickness_pages.insert(out.thickness_pages.end(), thickness_page.begin(),
                                               thickness_page.end());
                    ++out.page_blocks;
                }
            }
        }
    }
    return out;
}

// ---- the two chunks --------------------------------------------------------------------------

inline void append_u32(std::vector<u8>& out, u32 value) {
    out.push_back(static_cast<u8>(value & 0xFFu));
    out.push_back(static_cast<u8>((value >> 8) & 0xFFu));
    out.push_back(static_cast<u8>((value >> 16) & 0xFFu));
    out.push_back(static_cast<u8>((value >> 24) & 0xFFu));
}

inline std::vector<u8> material_chunk(const Volume& volume) {
    std::vector<u8> out;
    if (volume.empty()) return out;
    out.reserve(48 + volume.palette.size() * 2 + volume.index.size() * 4 +
                volume.material_pages.size());
    for (i32 axis = 0; axis < 3; ++axis) append_u32(out, static_cast<u32>(volume.dims[axis]));
    append_u32(out, static_cast<u32>(volume.cells_per_metre));
    append_u32(out, static_cast<u32>(volume.voxels_per_metre));
    append_u32(out, static_cast<u32>(kBlockDim));
    for (i32 axis = 0; axis < 3; ++axis) append_u32(out, static_cast<u32>(volume.blocks[axis]));
    append_u32(out, static_cast<u32>(volume.palette.size()));
    append_u32(out, static_cast<u32>(volume.index.size()));
    append_u32(out, static_cast<u32>(volume.page_blocks));
    for (const u16 value : volume.palette) {
        out.push_back(static_cast<u8>(value & 0xFFu));
        out.push_back(static_cast<u8>((value >> 8) & 0xFFu));
    }
    while ((out.size() & 3u) != 0) out.push_back(0);
    for (const u32 value : volume.index) append_u32(out, value);
    out.insert(out.end(), volume.material_pages.begin(), volume.material_pages.end());
    return out;
}

inline std::vector<u8> thickness_chunk(const Volume& volume) {
    std::vector<u8> out;
    if (volume.empty()) return out;
    out.reserve(8 + volume.thickness_pages.size());
    append_u32(out, static_cast<u32>(volume.page_blocks));
    append_u32(out, static_cast<u32>(volume.voxels_per_metre));
    out.insert(out.end(), volume.thickness_pages.begin(), volume.thickness_pages.end());
    return out;
}

}   // namespace ws::bake::matvol
